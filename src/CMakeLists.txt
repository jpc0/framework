add_executable(main)

target_sources(main PRIVATE main.cpp)

target_link_libraries(main fmt::fmt STDEXEC::stdexec Boost::fiber Boost::lockfree)
target_compile_features(main PRIVATE cxx_std_23)
