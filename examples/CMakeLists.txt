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
