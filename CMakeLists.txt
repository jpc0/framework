CMAKE_MINIMUM_REQUIRED(VERSION 3.28)

project(framework)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/CPM.cmake)

CPMAddPackage("gh:fmtlib/fmt#11.2.0")
CPMAddPackage(
  NAME Boost
  VERSION 1.88.0
  URL https://github.com/boostorg/boost/releases/download/boost-1.88.0/boost-1.88.0-cmake.tar.xz
  OPTIONS "BOOST_ENABLE_CMAKE ON" "BOOST_SKIP_INSTALL_RULES ON"
          "BUILD_SHARED_LIBS OFF" "BOOST_INCLUDE_LIBRARIES fiber\\\;lockfree"
  SYSTEM ON
)

CPMAddPackage(
  NAME stdexec
  GITHUB_REPOSITORY NVIDIA/stdexec
  GIT_TAG nvhpc-24.09
  OPTIONS "STDEXEC_BUILD_EXAMPLES OFF"
  SYSTEM ON
)

find_package(CURL REQUIRED)

add_subdirectory(src)
