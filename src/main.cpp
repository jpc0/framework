#include <fmt/base.h>
#include <fmt/format.h>

#include <boost/fiber/all.hpp>
#include <boost/fiber/operations.hpp>
#include <boost/lockfree/queue.hpp>
#include <chrono>
#include <csignal>
#include <exception>
#include <sstream>
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
                        stdexec::set_value(static_cast<R &&>(recv_));
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

auto main() -> int {
    using namespace std::chrono_literals;
    signal(SIGINT, catch_int);

    FiberExecutor ex{std::thread::hardware_concurrency()};
    std::atomic<bool> should_quit{false};
    auto sch = ex.get_scheduler();

    auto fun = [](int i) {
        auto thread_id =
            (std::stringstream{} << std::this_thread::get_id()).str();
        // fmt::println("running: {} on {}", i, thread_id);
        boost::this_fiber::sleep_for(10s);
        fmt::println("completed: {} on {}", i, thread_id);
    };
    auto main_thread_id =
        (std::stringstream{} << std::this_thread::get_id()).str();
    for (auto i = 0; i < 1'000'000; i++) {
        stdexec::start_detached(
            stdexec::start_on(sch, stdexec::just(i) | stdexec::then(fun)));
    }

    while (!should_quit) {
        int a;
        if (chan.pop(a)) {
            should_quit.store(true);
            ex.shutdown();
            break;
        }

        // Simulate some work
        /*
        fmt::println("\nMain Thread: {}", main_thread_id);
        auto work = stdexec::when_all(
            stdexec::start_on(sch, stdexec::just(0) | stdexec::then(fun)),
            stdexec::start_on(sch, stdexec::just(1) | stdexec::then(fun)),
            stdexec::start_on(sch, stdexec::just(2) | stdexec::then(fun)),
            stdexec::start_on(sch, stdexec::just(3) | stdexec::then(fun)),
            stdexec::start_on(sch, stdexec::just(4) | stdexec::then(fun)),
            stdexec::start_on(sch, stdexec::just(5) | stdexec::then(fun)),
            stdexec::start_on(sch, stdexec::just(6) | stdexec::then(fun)),
            stdexec::start_on(sch, stdexec::just(7) | stdexec::then(fun)));

        stdexec::sync_wait(std::move(work));
        */

        boost::this_fiber::yield();
        std::this_thread::sleep_for(10ms);
    }

    fmt::println("Exiting...");

    return 0;
}
