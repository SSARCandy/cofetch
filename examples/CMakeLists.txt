macro(add_example NAME)
    add_executable(${NAME} ${ARGN})
    target_link_libraries(${NAME} cofetch::cofetch)
endmacro(add_example)

add_example(example01
  example01.cpp
)

add_example(example02
  example02.cpp
)

add_example(example03
  example03.cpp
)

# Same source on asio's io_uring reactor, when liburing is around.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_library(URING_LIB uring)
    if(URING_LIB)
        add_example(example03_uring example03.cpp)
        target_compile_definitions(example03_uring
            PRIVATE ASIO_HAS_IO_URING ASIO_DISABLE_EPOLL)
        target_link_libraries(example03_uring ${URING_LIB})
    endif()
endif()
