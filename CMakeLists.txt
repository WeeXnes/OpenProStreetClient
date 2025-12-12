cmake_minimum_required(VERSION 3.10)
project(ProStreetRPC)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_library(ProStreetRPC SHARED library.cpp)

set_target_properties(ProStreetRPC PROPERTIES OUTPUT_NAME "ProStreetRPC")
set_target_properties(ProStreetRPC PROPERTIES SUFFIX ".asi")
