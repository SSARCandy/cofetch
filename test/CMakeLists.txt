INCLUDE_DIRECTORIES(${PROJECT_SOURCE_DIR}/googletest/googletest/include)

enable_testing()

macro(add_unittest TEST_NAME)
    add_executable(${TEST_NAME} ${ARGN})

    # Standard linking to gtest stuff.
    target_link_libraries(
        ${TEST_NAME}
        gtest
        gtest_main
        curl
        Threads::Threads
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endmacro(add_unittest)

add_unittest(all_test
  cofetch_tests.cpp
)