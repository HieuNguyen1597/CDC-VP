# fx1-ddr3 package. SystemC is located exactly as in the source build
# (fx1-ddr3-find-systemc.cmake): an existing SystemC::systemc target, SYSTEMC_HOME (CMake
# variable or environment), a SystemCLanguage package, or a header/library search.
include("${CMAKE_CURRENT_LIST_DIR}/fx1-ddr3-find-systemc.cmake")
if(NOT TARGET SystemC::systemc)
  set(fx1-ddr3_FOUND FALSE)
  set(fx1-ddr3_NOT_FOUND_MESSAGE
      "SystemC not found: set SYSTEMC_HOME (CMake variable or environment), add its prefix to CMAKE_PREFIX_PATH, or set SYSTEMC_INCLUDE_DIR and SYSTEMC_LIBRARY")
  return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/fx1-ddr3-targets.cmake")
