#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/urlapi.h>
#include <fmt/format.h>

#include <boost/fiber/all.hpp>
#include <boost/lockfree/queue.hpp>
#include <csignal>
#include <exception>
#include <stdexcept>
#include <stdexec/execution.hpp>
#include <thread>

static boost::lockfree::queue<int> chan{128};

auto catch_int(int sig) -> void {
    chan.push(1);
    signal(sig, SIG_DFL);
}

class FiberExecutor {
public:
    FiberExecutor(std::size_t thread_count) {
        threads_.reserve(thread_count);
        for (std::uint32_t i = 0; i < thread_count; ++i) {
            threads_.emplace_back(std::bind_front(&FiberExecutor::thread, this),
                                  thread_count);
        }
    }

    ~FiberExecutor() {
        shutdown();
    }

    template <typename F,
              typename... Args>
    void submit(F &&f,
                Args &&...args) {
        auto task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        auto status = task_channel_.push(std::move(task));
        if (status != boost::fibers::channel_op_status::success) {
            throw std::runtime_error(
                fmt::format("Failed to push task to channel: {}",
                            static_cast<int>(status)));
        }
    }

    void shutdown() {
        task_channel_.close();
    }

public:
    // Scheduler Impl

    struct scheduler_ {
        template <stdexec::receiver R>
        struct opstate_ {
            using operation_state_concept = stdexec::operation_state_t;
            R recv_;
            FiberExecutor *ex_;

            auto start() noexcept {
                ex_->submit([&] {
                    try {
                        if (stdexec::get_stop_token(static_cast<R &&>(recv_))
                                .stop_requested() ||
                            ex_->task_channel_.is_closed()) {
                            stdexec::set_stopped(static_cast<R &&>(recv_));
                        } else {
                            stdexec::set_value(static_cast<R &&>(recv_));
                        }
                    } catch (...) {
                        stdexec::set_error(static_cast<R &&>(recv_),
                                           std::current_exception());
                    }
                });
            }
        };
        struct sender_ {
            using sender_concept = stdexec::sender_t;
            using completion_signatures =
                stdexec::completion_signatures<stdexec::set_value_t(),
                                               stdexec::set_error_t(
                                                   std::exception_ptr)>;
            FiberExecutor *ex_;
            explicit sender_(FiberExecutor *ex) noexcept : ex_(ex) {
            }

            struct env_ {
                FiberExecutor *ex_;

                template <typename T>
                auto query(stdexec::get_completion_scheduler_t<T>)
                    const noexcept -> scheduler_ {
                    return ex_->get_scheduler();
                };
            };

            template <typename R>
            auto connect(R &&recv) const -> opstate_<R> {
                return opstate_{recv, ex_};
            };

            auto get_env() const noexcept -> env_ {
                return env_{ex_};
            }
        };

        using scheduler_concept = stdexec::schedule_t;
        FiberExecutor *ex_;

        auto schedule() {
            return sender_{ex_};
        }

        bool operator==(scheduler_ other) const noexcept {
            return other.ex_ == ex_;
        }
    };

    auto get_scheduler() -> scheduler_ {
        return scheduler_{this};
    };

private:
    friend scheduler_;
    void thread(std::uint32_t thread_count) {
        /* TODO: We cannot use work stealing because the scheduler spins on lock
        boost::fibers::use_scheduling_algorithm<
            boost::fibers::algo::work_stealing>(thread_count);
        */
        for (auto &task : task_channel_) {
            boost::fibers::fiber(std::move(task)).detach();
        }
    }

    boost::fibers::buffered_channel<std::function<void()>> task_channel_{128};
    std::vector<std::jthread> threads_;
};

auto write_callback(char *ptr,
                    size_t size,
                    size_t nmemb,
                    void *userdata) -> size_t {
    fmt::println("{}", std::string_view(ptr, size * nmemb));
    return size * nmemb;
};

auto header_callback(char *buffer,
                     size_t size,
                     size_t nitems,
                     void *userdata) -> size_t {
    fmt::print("size: {};nitems: {}\n\tHEADER: {}", size, nitems,
               std::string_view(buffer, size * nitems));
    return size * nitems;
};

class Request {
public:
    Request(std::string const &url) : handle_{curl_easy_init()} {
        if (!handle_) {
            throw std::runtime_error("Failed to init curl handle");
        }
        curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle_, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(handle_, CURLOPT_HEADERDATA, this);
        curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION,
                         Request::write_callback);
        curl_easy_setopt(handle_, CURLOPT_HEADERFUNCTION,
                         Request::header_callback);
    }

    ~Request() {
        curl_easy_cleanup(handle_);
    }

    auto make_request() {
        curl_easy_perform(handle_);
    }

    auto get_headers() -> std::span<char> {
        return headers_;
    }

    auto get_body() -> std::span<char> {
        return body_;
    }

    static auto write_callback(char *ptr,
                               size_t size,
                               size_t nmemb,
                               void *userdata) -> size_t {
        auto request = static_cast<Request *>(userdata);
        request->body_.reserve(size * nmemb);
        for (auto i : std::span(ptr, size * nmemb)) {
            request->body_.emplace_back(i);
        }
        return size * nmemb;
    }

    static auto header_callback(char *buffer,
                                size_t size,
                                size_t nitems,
                                void *userdata) -> size_t {
        auto request = static_cast<Request *>(userdata);
        request->headers_.reserve(size * nitems);
        for (auto i : std::span(buffer, size * nitems)) {
            request->headers_.emplace_back(i);
        }
        return size * nitems;
    };

private:
    std::vector<char> body_;
    std::vector<char> headers_;
    CURL *handle_;
};

auto main() -> int {
    using namespace std::chrono_literals;
    signal(SIGINT, catch_int);

    FiberExecutor ex{std::thread::hardware_concurrency()};
    std::atomic<bool> should_quit{false};
    auto sch = ex.get_scheduler();

    {
        Request req{"https://www.example.com/api"};
        req.make_request();
        fmt::println("Headers:\n{}\n", std::string_view(req.get_headers()));
        fmt::println("Body:\n{}\n", std::string_view(req.get_body()));
    }

    while (!should_quit) {
        int a;
        if (chan.pop(a)) {
            should_quit.store(true);
            ex.shutdown();
            break;
        }

        boost::this_fiber::yield();
        std::this_thread::sleep_for(10ms);
    }

    fmt::println("Exiting...");

    return 0;
}
