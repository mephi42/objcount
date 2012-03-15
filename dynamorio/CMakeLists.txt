cmake_minimum_required(VERSION 2.8)
find_package(DynamoRIO)

add_definitions(-g3)
add_library(objcount SHARED objcount.c)
use_DynamoRIO_extension(objcount drcontainers)
use_DynamoRIO_extension(objcount drsyms)
use_DynamoRIO_extension(objcount drwrap)
configure_DynamoRIO_client(objcount)
target_link_libraries(objcount -liberty_pic -Wl,-no-undefined)

add_executable(test-app test-app.cpp)
set_property(SOURCE test-app.cpp PROPERTY COMPILE_FLAGS "-std=c++0x -g3 -fno-inline")
