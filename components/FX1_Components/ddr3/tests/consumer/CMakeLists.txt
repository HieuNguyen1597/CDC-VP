# Package consumer: builds against an installed fx1-ddr3 package using only public headers.
cmake_minimum_required(VERSION 3.16)
project(fx1_ddr3_consumer CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(fx1-ddr3 0.1 REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE cdc::components::fx1_ddr3)
