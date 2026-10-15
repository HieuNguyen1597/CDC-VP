# Standalone convenience wrapper; the vendor tools on PATH shadow g++, so the host
# GCC is pinned explicitly.
CMAKE ?= cmake
CTEST ?= ctest
BUILD_DIR ?= build
# Plain '=' on purpose: the login environment may export SYSTEMC_HOME for another
# SystemC (e.g. ARM Fast Models). Override on the command line: make SYSTEMC_HOME=...
SYSTEMC_HOME = /opt/systemc-2.3.4
CMAKE_ARGS ?=
export CC := /usr/bin/gcc
export CXX := /usr/bin/g++

.PHONY: all configure test sanitize clean
all: configure
	$(CMAKE) --build "$(BUILD_DIR)" --parallel 2
configure:
	$(CMAKE) -S . -B "$(BUILD_DIR)" -G Ninja -DSYSTEMC_HOME="$(SYSTEMC_HOME)" -DFX1_DDR3_BUILD_TESTS=ON $(CMAKE_ARGS)
test: all
	$(CTEST) --test-dir "$(BUILD_DIR)" --output-on-failure --no-tests=error
sanitize:
	$(MAKE) test BUILD_DIR=build-asan CMAKE_ARGS="-DFX1_DDR3_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug"
clean:
	rm -rf "$(BUILD_DIR)" build-asan
