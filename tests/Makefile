# A POSIX host compiler is sufficient; no Switch SDK or test framework required.
CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -pthread
ROOT := ..
OUT ?= /tmp/cyberfoil-lan-discovery-tests
.PHONY: test
test:
	$(CXX) $(CXXFLAGS) -Isave_data_stubs -I$(ROOT)/include save_data_policy_tests.cpp -o $(OUT)-save-data
	$(OUT)-save-data
	$(CXX) $(CXXFLAGS) -I$(ROOT)/include file_selection_tests.cpp -o $(OUT)-selection
	$(OUT)-selection
	$(CXX) $(CXXFLAGS) -I$(ROOT)/include ui_dispatcher_tests.cpp -o $(OUT)-ui
	$(OUT)-ui
	$(CXX) $(CXXFLAGS) -I$(ROOT)/include install_queue_tests.cpp -o $(OUT)-queue
	$(OUT)-queue
	$(CXX) $(CXXFLAGS) -I$(ROOT)/include lan_discovery_tests.cpp $(ROOT)/source/util/lan_discovery.cpp $(ROOT)/source/util/lan_discovery_scan.cpp -o $(OUT)
	$(OUT)
	$(CXX) $(CXXFLAGS) -DAPP_VERSION=\"test\" -I$(ROOT)/include remote_connection_tests.cpp $(ROOT)/source/util/config.cpp -o $(OUT)-remote
	$(OUT)-remote
	@# The theme loader needs host SDL2 and SDL2_image; skip rather than fail without them.
	@if pkg-config --exists sdl2 SDL2_image; then \
		$(CXX) $(CXXFLAGS) -Itheme_stubs -I$(ROOT)/include theme_tests.cpp -o $(OUT)-theme $$(pkg-config --cflags --libs sdl2 SDL2_image) && $(OUT)-theme; \
	else echo "theme tests skipped: install SDL2 and SDL2_image development packages"; fi
