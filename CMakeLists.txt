cmake_minimum_required(VERSION 3.12.4...4.2.3)

project(utxt)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS on)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/wall.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/no-rtti.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/no-exceptions.cmake)

if(UTXT_ENABLE_ASAN)
  include(${CMAKE_CURRENT_LIST_DIR}/cmake/asan.cmake)
endif()

add_library(stb_truetype src/stb_truetype.c)

add_library(utxt src/utxt.cpp)
target_include_directories(utxt PUBLIC include/)
target_link_libraries(utxt PRIVATE stb_truetype)
utxt_set_wall(utxt)
utxt_set_no_exceptions(utxt)
utxt_set_no_rtti(utxt)

# This will only be true if this project is not used as a subdirectory (e.g. FetchContent)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  option(UTXT_BUILD_EXAMPLES "Build Examples" ON)

  if(UTXT_BUILD_EXAMPLES)
    add_executable(utxt-example example.cpp)
    target_link_libraries(utxt-example PRIVATE utxt)
    utxt_set_wall(utxt-example)
    utxt_set_no_exceptions(utxt-example)
    utxt_set_no_rtti(utxt-example)
  endif()
endif()
