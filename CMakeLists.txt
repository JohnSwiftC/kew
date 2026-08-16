cmake_minimum_required(VERSION 4.2)
project(kew)

set(CMAKE_CXX_STANDARD 14)

include_directories(.)

add_executable(kew
        service.cpp
        service.h)
