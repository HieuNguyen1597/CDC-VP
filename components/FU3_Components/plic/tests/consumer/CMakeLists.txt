cmake_minimum_required(VERSION 3.16)
project(plic_consumer LANGUAGES CXX)

find_package(fu3-plic CONFIG REQUIRED)

add_executable(plic_consumer ../behavior.cpp)
target_link_libraries(plic_consumer PRIVATE fu3::plic)

enable_testing()

add_test(NAME installed_plic_behavior COMMAND plic_consumer)
set_tests_properties(installed_plic_behavior PROPERTIES TIMEOUT 30)
