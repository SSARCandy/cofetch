cmake_minimum_required(VERSION 3.14)

project(cofetch VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(COFETCH_TOP_LEVEL ON)
else()
    set(COFETCH_TOP_LEVEL OFF)
endif()

option(COFETCH_USE_VENDORED_ASIO "Use the asio submodule under vendor/" ${COFETCH_TOP_LEVEL})
option(COFETCH_USE_BOOST_ASIO "Build against Boost.Asio instead of standalone asio" OFF)
option(COFETCH_BUILD_EXAMPLES "Build example programs" ${COFETCH_TOP_LEVEL})
option(COFETCH_BUILD_TESTS "Build unit tests (needs vendor/googletest)" OFF)
option(COFETCH_BUILD_BENCH "Build benchmarks (fetches cpr and cpp-httplib)" OFF)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_library(cofetch INTERFACE)
add_library(cofetch::cofetch ALIAS cofetch)
target_include_directories(cofetch INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Consumers need C++17; the co_await interface lights up on C++20.
target_compile_features(cofetch INTERFACE cxx_std_17)
target_link_libraries(cofetch INTERFACE CURL::libcurl Threads::Threads)

if(COFETCH_USE_BOOST_ASIO)
    find_package(Boost REQUIRED)
    target_link_libraries(cofetch INTERFACE Boost::headers)
    target_compile_definitions(cofetch INTERFACE COFETCH_USE_BOOST_ASIO)
elseif(COFETCH_USE_VENDORED_ASIO)
    target_include_directories(cofetch SYSTEM INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/asio/include>)
endif()

if(COFETCH_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    add_compile_options(-Wall -Wextra)

    if(NOT CMAKE_BUILD_TYPE)
        message(STATUS "No build type specified, defaulting to Release")
        set(CMAKE_BUILD_TYPE "Release")
    endif()

    if(CMAKE_BUILD_TYPE MATCHES Debug AND "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        message(STATUS "Debug mode, enable gcov")
        add_compile_options(-fprofile-arcs -ftest-coverage)
        add_link_options(-fprofile-arcs -ftest-coverage)
    endif()
endif()

if(COFETCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(vendor/googletest)
    add_subdirectory(test)
endif()

if(COFETCH_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(COFETCH_BUILD_BENCH)
    add_subdirectory(bench)
endif()
