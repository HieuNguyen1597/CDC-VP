# FTEL DDR3 controller VP model.
# Builds standalone (own project, CTest, installable package) or as a CDC-VP
# subproject (fx1_unit tests, cdc-components-targets export).
cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(fx1_ddr3 VERSION 0.1.0 LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
  endif()
  include(CTest)
  set(_ddr3_tests_default ${BUILD_TESTING})
elseif(DEFINED CDC_BUILD_TESTS)
  set(_ddr3_tests_default ${CDC_BUILD_TESTS})
else()
  set(_ddr3_tests_default OFF)
endif()

option(FX1_DDR3_BUILD_TESTS "Build the DDR3 controller regressions" ${_ddr3_tests_default})
option(FX1_DDR3_SANITIZE "Build the DDR3 libraries and tests with ASan/UBSan" OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/fx1-ddr3-find-systemc.cmake)
if(NOT TARGET SystemC::systemc)
  message(FATAL_ERROR "SystemC not found. Set SYSTEMC_HOME (CMake variable or environment), add its prefix to "
                      "CMAKE_PREFIX_PATH, or set SYSTEMC_INCLUDE_DIR and SYSTEMC_LIBRARY.")
endif()

set(_ddr3_warnings -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
set(_ddr3_sanitize -fsanitize=address,undefined -fno-omit-frame-pointer)

# Pure C++17 logic (profiles, speed bins, AXI vocabulary, CSR map, frontend decode,
# address mapping, ECC codec, bank timing, sparse store, exclusive monitor): no SystemC.
add_library(fx1_ddr3_core
  src/profile.cpp
  src/speed_bin.cpp
  src/axi_types.cpp
  src/config_checks.cpp
  src/csr_map.cpp
  src/frontend.cpp
  src/address_map.cpp
  src/ecc_codec.cpp
  src/bank_timer.cpp
  src/granule_store.cpp
  src/exclusive_monitor.cpp)
add_library(cdc::components::fx1_ddr3_core ALIAS fx1_ddr3_core)
target_compile_features(fx1_ddr3_core PUBLIC cxx_std_17)
target_include_directories(fx1_ddr3_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)

# SystemC/TLM model.
add_library(fx1_ddr3
  src/epoch_scheduler.cpp
  src/functional_phy.cpp
  src/ddr3_controller.cpp
  src/ddr3_csr.cpp
  src/ddr3_lifecycle.cpp
  src/ddr3_memory.cpp
  src/generic_axi_bridge.cpp)
add_library(cdc::components::fx1_ddr3 ALIAS fx1_ddr3)
target_link_libraries(fx1_ddr3 PUBLIC fx1_ddr3_core SystemC::systemc)

foreach(_t fx1_ddr3_core fx1_ddr3)
  set_target_properties(${_t} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(NOT MSVC)
    target_compile_options(${_t} PRIVATE ${_ddr3_warnings})
    if(FX1_DDR3_SANITIZE)
      target_compile_options(${_t} PUBLIC ${_ddr3_sanitize})
      target_link_options(${_t} PUBLIC ${_ddr3_sanitize})
    endif()
  endif()
endforeach()

if(FX1_DDR3_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CMAKE_PROJECT_NAME STREQUAL "cdc-vp")
  install(TARGETS fx1_ddr3_core fx1_ddr3 EXPORT cdc-components-targets ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
else()
  include(CMakePackageConfigHelpers)
  install(TARGETS fx1_ddr3_core fx1_ddr3 EXPORT fx1-ddr3-targets ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
  install(EXPORT fx1-ddr3-targets NAMESPACE cdc::components:: DESTINATION lib/cmake/fx1-ddr3)
  write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/fx1-ddr3-config-version.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
  install(FILES cmake/fx1-ddr3-config.cmake cmake/fx1-ddr3-find-systemc.cmake
    "${CMAKE_CURRENT_BINARY_DIR}/fx1-ddr3-config-version.cmake" DESTINATION lib/cmake/fx1-ddr3)
endif()
install(DIRECTORY include/ DESTINATION include)
