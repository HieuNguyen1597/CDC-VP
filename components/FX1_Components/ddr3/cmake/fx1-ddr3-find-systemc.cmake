# SystemC discovery shared by the source build and the installed fx1-ddr3 package, so both
# accept the same inputs. Defines the SystemC::systemc target when SystemC is found.
#   1. An existing SystemC::systemc target (for example a parent project's) is reused.
#   2. SYSTEMC_HOME, as a CMake variable or else from the environment, is searched first
#      and exclusively: its SystemCLanguage package, else its header and library (an
#      installation without CMake metadata).
#   3. Otherwise the default search: a SystemCLanguage package on CMAKE_PREFIX_PATH and the
#      system paths, else a header/library search there (SYSTEMC_INCLUDE_DIR and
#      SYSTEMC_LIBRARY may also be given directly).
if(TARGET SystemC::systemc)
  return()
endif()

macro(_fx1_ddr3_systemc_target)
  if(NOT TARGET SystemC::systemc AND SYSTEMC_INCLUDE_DIR AND SYSTEMC_LIBRARY)
    find_package(Threads QUIET)
    add_library(SystemC::systemc UNKNOWN IMPORTED)
    set_target_properties(SystemC::systemc PROPERTIES IMPORTED_LOCATION "${SYSTEMC_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${SYSTEMC_INCLUDE_DIR}")
    if(TARGET Threads::Threads)
      set_property(TARGET SystemC::systemc PROPERTY INTERFACE_LINK_LIBRARIES Threads::Threads)
    endif()
  endif()
endmacro()

set(_fx1_ddr3_sc_home "")
if(SYSTEMC_HOME)
  set(_fx1_ddr3_sc_home "${SYSTEMC_HOME}")
elseif(DEFINED ENV{SYSTEMC_HOME})
  set(_fx1_ddr3_sc_home "$ENV{SYSTEMC_HOME}")
endif()

if(_fx1_ddr3_sc_home)
  find_package(SystemCLanguage CONFIG QUIET PATHS "${_fx1_ddr3_sc_home}" NO_DEFAULT_PATH)
  if(NOT TARGET SystemC::systemc)
    find_path(SYSTEMC_INCLUDE_DIR NAMES systemc PATHS "${_fx1_ddr3_sc_home}" PATH_SUFFIXES include NO_DEFAULT_PATH)
    find_library(SYSTEMC_LIBRARY NAMES systemc PATHS "${_fx1_ddr3_sc_home}" PATH_SUFFIXES lib64 lib lib-linux64
                 NO_DEFAULT_PATH)
    _fx1_ddr3_systemc_target()
  endif()
endif()

if(NOT TARGET SystemC::systemc)
  find_package(SystemCLanguage CONFIG QUIET)
endif()
if(NOT TARGET SystemC::systemc)
  find_path(SYSTEMC_INCLUDE_DIR NAMES systemc PATH_SUFFIXES include)
  find_library(SYSTEMC_LIBRARY NAMES systemc PATH_SUFFIXES lib64 lib lib-linux64)
  _fx1_ddr3_systemc_target()
endif()
unset(_fx1_ddr3_sc_home)
