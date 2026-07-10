include(FetchContent)

set(CPR_USE_SYSTEM_CURL ON CACHE BOOL "" FORCE)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG 1.11.2
    GIT_SHALLOW TRUE)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.20.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(cpr httplib)

add_executable(bench_cofetch bench_cofetch.cpp)
target_link_libraries(bench_cofetch cofetch::cofetch)

add_executable(bench_cpr bench_cpr.cpp)
target_link_libraries(bench_cpr cpr::cpr)

add_executable(bench_httplib bench_httplib.cpp)
target_link_libraries(bench_httplib httplib::httplib)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(bench_epoll bench_epoll.cpp)
    target_link_libraries(bench_epoll CURL::libcurl Threads::Threads)
endif()
