cmake_minimum_required(VERSION 4.1)

project(kraft C)

if (TARGET ${PROJECT_NAME})
    return()
endif()

add_subdirectory(deps)
add_subdirectory(src)
add_subdirectory(src/demo)
add_subdirectory(src/loopy-demo)

# vi:ai et sw=4 ts=4:
