macro(add_example NAME)
    add_executable(${NAME} ${ARGN})
    target_link_libraries(${NAME} cofetch::cofetch)
endmacro(add_example)

add_example(example_hello example_hello.cpp)
add_example(example_fluent example_fluent.cpp)
add_example(example_coroutine example_coroutine.cpp)
add_example(example_future example_future.cpp)
add_example(example_concurrent example_concurrent.cpp)
add_example(example_errors example_errors.cpp)
add_example(example_cancellation example_cancellation.cpp)
add_example(example_reactors example_reactors.cpp)

# Same source on asio's io_uring reactor, when liburing is around.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_library(URING_LIB uring)
    if(URING_LIB)
        add_example(example_reactors_uring example_reactors.cpp)
        target_compile_definitions(example_reactors_uring
            PRIVATE ASIO_HAS_IO_URING ASIO_DISABLE_EPOLL)
        target_link_libraries(example_reactors_uring ${URING_LIB})
    endif()
endif()
