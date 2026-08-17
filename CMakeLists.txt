cmake_minimum_required(VERSION 4.2)
project(kew)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(.)

add_executable(kew
        service.cpp
        service.h)
