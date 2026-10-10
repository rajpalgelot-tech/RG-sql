cmake_minimum_required(VERSION 3.10)
project(rajsql CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_library(rajsql_lib STATIC
  src/value.cpp
  src/lexer.cpp
  src/parser.cpp
  src/pager.cpp
  src/wal.cpp
  src/bptree.cpp
  src/database.cpp
)
target_include_directories(rajsql_lib PUBLIC src)

add_executable(rajsql_tests tests/test_all.cpp)
target_link_libraries(rajsql_tests PRIVATE rajsql_lib)

add_executable(rajsql_cli src/cli/main.cpp)
target_link_libraries(rajsql_cli PRIVATE rajsql_lib)

enable_testing()
add_test(NAME rajsql_tests COMMAND rajsql_tests)
