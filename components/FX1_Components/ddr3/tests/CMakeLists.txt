# DDR3 controller regressions. Labels: fx1_unit (CDC-VP smoke stage) and ddr3.
set(_ddr3_labels "fx1;fx1_unit;ddr3")

function(fx1_ddr3_cases exe)
  foreach(case ${ARGN})
    add_test(NAME fx1_ddr3_${exe}_${case} COMMAND fx1_ddr3_test_${exe} ${case})
    set_tests_properties(fx1_ddr3_${exe}_${case} PROPERTIES TIMEOUT 60 LABELS "${_ddr3_labels}")
  endforeach()
endfunction()

# Core logic: no SystemC.
add_executable(fx1_ddr3_test_core test_core.cpp)
target_link_libraries(fx1_ddr3_test_core PRIVATE fx1_ddr3_core)
fx1_ddr3_cases(core profile capability speed_bin dfi_bounds config_checks timebase burst_plan)
add_executable(fx1_ddr3_test_datapath_core test_datapath_core.cpp)
target_link_libraries(fx1_ddr3_test_datapath_core PRIVATE fx1_ddr3_core)
fx1_ddr3_cases(datapath_core ecc_codec address_map frontend_decode atom_plan exclusive_monitor granule_store
  bank_timer)

# SystemC/TLM skeleton: binding, identity CSR, PHY provider, epoch scheduler.
add_executable(fx1_ddr3_test_tlm test_tlm.cpp)
target_link_libraries(fx1_ddr3_test_tlm PRIVATE fx1_ddr3)
fx1_ddr3_cases(tlm bind_profiles identity bad_port_binding no_extension csr_robustness reset_contract
  phy_handshake phy_faults epoch)

# Subtask 2: CSR boundary and lifecycle.
add_executable(fx1_ddr3_test_csr test_csr.cpp)
target_link_libraries(fx1_ddr3_test_csr PRIVATE fx1_ddr3)
fx1_ddr3_cases(csr inventory commits mutex_snapshot irq irq_test_disabled policy fe_port)
add_executable(fx1_ddr3_test_lifecycle test_lifecycle.cpp)
target_link_libraries(fx1_ddr3_test_lifecycle PRIVATE fx1_ddr3)
fx1_ddr3_cases(lifecycle boot_ecc boot_no_ecc boot_legacy init_fail phy_failure runtime_training commit_quiesce
  power refresh cancel_training cancel_power phy_failure_training cancel_fe_port cancel_snapshot train_fields)

# Subtask 3: memory data path, ECC and error handling.
add_executable(fx1_ddr3_test_memory test_memory.cpp)
target_link_libraries(fx1_ddr3_test_memory PRIVATE fx1_ddr3)
fx1_ddr3_cases(memory data_r2 data_r1 data_legacy data_noecc decode ordering ordering_noecc exclusive exclusive_power exclusive_race
  exclusive_race_noecc admission multiport capacity
  timing debug bridge bridge_legacy)

add_executable(fx1_ddr3_test_ecc test_ecc.cpp)
target_link_libraries(fx1_ddr3_test_ecc PRIVATE fx1_ddr3)
fx1_ddr3_cases(ecc ecc ecc_legacy ecc_off scrub quarantine quarantine_walk quarantine_reasons fatal fatal_noecc
  fatal_containment fatal_containment_noecc fatal_followers fatal_sources reset)

# Subtask 4: conformance, seeded soak, efficiency.
add_executable(fx1_ddr3_test_verify test_verify.cpp)
target_link_libraries(fx1_ddr3_test_verify PRIVATE fx1_ddr3)
fx1_ddr3_cases(verify csr_during_commit levels clock_gating soft_reset_backend commit_not_idle ack_blocker
  core_offline_map snapshot_no_pause irq_vs_sticky admission_block wake_no_retrain soak soak_legacy soak_noecc
  idle_events perf)
# The soak is reproducible: the same seed gives the same response digest.
add_test(NAME fx1_ddr3_verify_soak_reproducible
  COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:fx1_ddr3_test_verify> -DCASE=soak -DSEED=7
          -P ${CMAKE_CURRENT_SOURCE_DIR}/check_reproducible.cmake)
set_tests_properties(fx1_ddr3_verify_soak_reproducible PROPERTIES TIMEOUT 120 LABELS "${_ddr3_labels}")

# Standalone package: install and build a consumer against the exported fx1-ddr3 package.
if(NOT CMAKE_PROJECT_NAME STREQUAL "cdc-vp")
  # The fixture uses the SystemC the producer actually resolved (whatever input found it):
  # its header directory, its library file, and its installation root (from the CMake
  # package when there is one, else the parent of the header directory).
  get_target_property(_ddr3_sc_inc SystemC::systemc INTERFACE_INCLUDE_DIRECTORIES)
  list(GET _ddr3_sc_inc 0 _ddr3_sc_inc)
  get_target_property(_ddr3_sc_lib SystemC::systemc LOCATION)
  if(SystemCLanguage_DIR)
    get_filename_component(_ddr3_sc_root "${SystemCLanguage_DIR}/../../.." ABSOLUTE)
  else()
    get_filename_component(_ddr3_sc_root "${_ddr3_sc_inc}" DIRECTORY)
  endif()
  # One case per documented SystemC discovery method (docs/integration_guide.md §1).
  foreach(_mode prefix home_var home_env bare_home bare_prefix_path explicit)
    add_test(NAME fx1_ddr3_package_consumer_${_mode}
      COMMAND ${CMAKE_COMMAND} -DBUILD=${PROJECT_BINARY_DIR} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/consumer
              -DWORK=${CMAKE_CURRENT_BINARY_DIR}/package-consumer-${_mode} -DGEN=${CMAKE_GENERATOR}
              -DSC_ROOT=${_ddr3_sc_root} -DSC_INC=${_ddr3_sc_inc} -DSC_LIB=${_ddr3_sc_lib}
              -DCXX=${CMAKE_CXX_COMPILER} -DMODE=${_mode}
              -P ${CMAKE_CURRENT_SOURCE_DIR}/check_consumer.cmake)
    set_tests_properties(fx1_ddr3_package_consumer_${_mode} PROPERTIES TIMEOUT 300 LABELS "ddr3;ddr3_package")
  endforeach()
endif()

# The firmware register header must compile as plain C99.
include(CheckLanguage)
check_language(C)
if(CMAKE_C_COMPILER)
  enable_language(C)
  add_executable(fx1_ddr3_test_regs_header_c test_regs_header.c)
  target_include_directories(fx1_ddr3_test_regs_header_c PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
  set_target_properties(fx1_ddr3_test_regs_header_c PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(NOT MSVC)
    target_compile_options(fx1_ddr3_test_regs_header_c PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME fx1_ddr3_regs_header_c COMMAND fx1_ddr3_test_regs_header_c)
  set_tests_properties(fx1_ddr3_regs_header_c PROPERTIES TIMEOUT 10 LABELS "${_ddr3_labels}")
endif()

foreach(_t fx1_ddr3_test_core fx1_ddr3_test_datapath_core fx1_ddr3_test_tlm fx1_ddr3_test_csr fx1_ddr3_test_lifecycle
    fx1_ddr3_test_memory fx1_ddr3_test_ecc fx1_ddr3_test_verify)
  if(NOT MSVC)
    target_compile_options(${_t} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()
