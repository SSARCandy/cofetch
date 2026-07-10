macro(add_unittest TEST_NAME)
    add_executable(${TEST_NAME} ${ARGN})
    target_link_libraries(
        ${TEST_NAME}
        cofetch::cofetch
        gtest
        gtest_main
    )
    target_compile_definitions(${TEST_NAME}
        PRIVATE COFETCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endmacro(add_unittest)

add_unittest(all_test
  cofetch_tests.cpp
)
