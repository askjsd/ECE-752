#!/usr/bin/env bash
set -euo pipefail

export WARM_START_TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
build="${1:-$HOME/champsim-knobbed-linux}"
if [[ ! -f "$build/_configuration.mk" ]]; then
    echo "Build ChampSim with champsim_config_entangling.json and pass its build directory." >&2
    exit 1
fi

make -C "$build" --no-print-directory -f Makefile -f - check-warm-start <<'MAKE'
ifeq ($(filter $(BIN_ROOT)/champsim_entangling,$(executable_name)),)
$(error Configure this build with champsim_config_entangling.json)
endif
ifneq ($(words $(build_ids)),1)
$(error Configure exactly one simulator in this build directory)
endif

test_objects := $(sort $(filter-out %_main.o,$(call get_base_objs,$(build_ids))) $(base_module_objs) $(nonbase_module_objs))

.PHONY: check-warm-start
check-warm-start: $(BIN_ROOT)/champsim_entangling
	$(CXX) @absolute.options @global.options $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -DCHAMPSIM_BUILD=0x$(build_ids) "$(WARM_START_TEST_DIR)/test_apply_region_settings_live.cpp" $(test_objects) $(LOADLIBES) $(LDLIBS) -o $(BIN_ROOT)/test_apply_region_settings_live
	$(BIN_ROOT)/test_apply_region_settings_live
MAKE
