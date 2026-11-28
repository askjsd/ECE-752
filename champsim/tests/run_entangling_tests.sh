#!/usr/bin/env bash
# Use the same objects, generated configuration and libraries as the simulator.
set -euo pipefail
export ENTANGLING_TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
build="${1:-$HOME/champsim-knobbed-linux}"
if [[ ! -f "$build/_configuration.mk" ]]; then
    echo "Build champsim_config_entangling.json first and pass the build directory." >&2
    exit 1
fi

# These checks start at the built-in defaults regardless of the caller's shell.
unset EIP_ENABLED EIP_CONFIDENCE_THRESHOLD EIP_DESTINATION_CAP
unset EDP_ENABLED EDP_MAX_REQUESTS_PER_TARGET EDP_L1_FILL_THRESHOLD_PERCENT

make -C "$build" --no-print-directory -f Makefile -f - check-entangling <<'MAKE'
ifeq ($(filter $(BIN_ROOT)/champsim_entangling,$(executable_name)),)
$(error Configure this build with champsim_config_entangling.json)
endif
ifneq ($(words $(build_ids)),1)
$(error Configure exactly one simulator in this build directory)
endif

test_objects := $(sort $(filter-out %_main.o,$(call get_base_objs,$(build_ids))) $(base_module_objs) $(nonbase_module_objs))
test_cxx = $(CXX) @absolute.options @global.options $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS)
test_libs = $(LOADLIBES) $(LDLIBS)

.PHONY: check-entangling
check-entangling: $(BIN_ROOT)/champsim_entangling
	$(test_cxx) "$(ENTANGLING_TEST_DIR)/test_entangling.cc" $(filter-out %/entangling.o,$(test_objects)) $(test_libs) -o $(BIN_ROOT)/test_entangling
	$(BIN_ROOT)/test_entangling
	$(test_cxx) "$(ENTANGLING_TEST_DIR)/test_edp_history.cc" $(filter-out %/edp.o,$(test_objects)) $(test_libs) -o $(BIN_ROOT)/test_edp_history
	$(BIN_ROOT)/test_edp_history
	$(test_cxx) -DCHAMPSIM_BUILD=0x$(build_ids) "$(ENTANGLING_TEST_DIR)/test_entangling_runtime.cc" $(test_objects) $(test_libs) -o $(BIN_ROOT)/test_entangling_runtime
	$(BIN_ROOT)/test_entangling_runtime
MAKE
