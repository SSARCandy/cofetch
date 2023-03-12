macro(add_example NAME)
    add_executable(${NAME} ${ARGN})

    # Standard linking to gtest stuff.
    target_link_libraries(
        ${NAME}
        curl
    )
    add_test(NAME ${NAME} COMMAND ${NAME})
endmacro(add_example)

add_example(example01
  example01.cpp 
  ../simdjson/singleheader/simdjson.cpp
)