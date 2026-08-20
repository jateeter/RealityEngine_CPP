CXX ?= g++
BOOST_PREFIX ?= $(shell brew --prefix boost 2>/dev/null)
OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null)
BOOST_CPPFLAGS ?= $(if $(BOOST_PREFIX),-I$(BOOST_PREFIX)/include,)
OPENSSL_CPPFLAGS ?= $(if $(OPENSSL_PREFIX),-I$(OPENSSL_PREFIX)/include,)
OPENSSL_LDFLAGS ?= $(if $(OPENSSL_PREFIX),-L$(OPENSSL_PREFIX)/lib,)

# macOS: the Command Line Tools' bundled libc++ headers can be incomplete or
# stale, leaving the toolchain's /usr/include/c++/v1 missing umbrella headers
# such as <atomic> and <cctype> (the search order puts that incomplete dir ahead
# of the SDK's complete copy). Anchor the build to the active SDK and add its
# libc++ include path so standard headers resolve reliably. No effect off macOS.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  SDKROOT ?= $(shell xcrun --show-sdk-path 2>/dev/null)
  MACOS_CXXFLAGS := $(if $(SDKROOT),-isysroot $(SDKROOT) -isystem $(SDKROOT)/usr/include/c++/v1,)
endif

CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -pedantic -Iinclude $(BOOST_CPPFLAGS) $(OPENSSL_CPPFLAGS) $(MACOS_CXXFLAGS)
LDFLAGS ?= -pthread $(OPENSSL_LDFLAGS) -lssl -lcrypto

# Header dependency generation. Kept out of CXXFLAGS deliberately: CXXFLAGS is
# `?=` and callers override it, which would otherwise silently drop dependency
# tracking and restore the failure mode described below.
#
# Without this, editing a header rebuilt nothing. `make` compared each binary
# only against the .cpp files listed as its prerequisites, found them older, and
# reported "up to date" — so a changed include/reality/*.hpp produced a stale
# binary and a green build. That is worse than a slow build: it reports success
# for something it did not build. Caught when a fix to
# include/reality/vector_aggregator.hpp was verified against a binary compiled
# 40 minutes before the change (RealityEngine_CI corpus parity sweep,
# 2026-08-19).
DEPFLAGS := -MMD -MP

BIN_DIR := bin
OBJ_DIR := build
SRC := src/reality.cpp src/arbiter.cpp src/http.cpp src/sta_checker.cpp src/mqtt_client.cpp src/mqtt_mapping.cpp src/mqtt_bridge.cpp

# Every binary used to compile all of SRC from source, so `make all` compiled
# the same seven engine files once per binary — about 140 compilations at -O2
# for 20 binaries. They are now compiled once into objects and linked.
SRC_OBJ := $(SRC:%.cpp=$(OBJ_DIR)/%.o)

SERVER_OBJ := $(OBJ_DIR)/src/reality_engine_server.o $(OBJ_DIR)/src/perception_engine_server.o
CLI_OBJ    := $(OBJ_DIR)/src/http.o $(OBJ_DIR)/tools/reality_engine_cli.o

TEST_NAMES := reality_engine_tests arbiter_tests sta_checker_tests mqtt_client_tests \
              mqtt_mapping_tests e2e_machine_sequences e2e_machine_domains \
              e2e_domain_scenarios e2e_ai_trigger_dispatch e2e_yuma_localai_cascade \
              cesgen_oracles_parity cesgen_provenance cesgen_composition \
              cesgen_governance cesgen_contracts_parity cesgen_deprecation
TEST_OBJ   := $(TEST_NAMES:%=$(OBJ_DIR)/tests/%.o) $(OBJ_DIR)/tests/cesgen_index_compile.o

ALL_OBJ := $(SRC_OBJ) $(SERVER_OBJ) $(CLI_OBJ) $(TEST_OBJ)
DEPS    := $(ALL_OBJ:.o=.d)

.PHONY: all clean test e2e e2e-corpus e2e-services e2e-healthkit-spezi

all: $(BIN_DIR)/reality_engine_server $(BIN_DIR)/perception_engine_server $(BIN_DIR)/reality_engine_cli $(TEST_NAMES:%=$(BIN_DIR)/%) $(BIN_DIR)/cesgen_index_compile

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# One compile rule for every translation unit. `mkdir -p $(@D)` mirrors the
# source tree under $(OBJ_DIR) so src/, tests/ and tools/ can share it.
$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Link only.
$(BIN_DIR)/reality_engine_server: $(SRC_OBJ) $(OBJ_DIR)/src/reality_engine_server.o | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/perception_engine_server: $(SRC_OBJ) $(OBJ_DIR)/src/perception_engine_server.o | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/reality_engine_cli: $(CLI_OBJ) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# cesgen_index_compile is a standalone translation unit: no engine sources and
# no OpenSSL, matching its previous rule.
$(BIN_DIR)/cesgen_index_compile: $(OBJ_DIR)/tests/cesgen_index_compile.o | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@

# Every remaining test binary is the engine objects plus its own object.
$(TEST_NAMES:%=$(BIN_DIR)/%): $(BIN_DIR)/%: $(SRC_OBJ) $(OBJ_DIR)/tests/%.o | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

test: $(BIN_DIR)/reality_engine_tests $(BIN_DIR)/arbiter_tests $(BIN_DIR)/sta_checker_tests $(BIN_DIR)/mqtt_client_tests $(BIN_DIR)/mqtt_mapping_tests
	$(BIN_DIR)/reality_engine_tests
	$(BIN_DIR)/sta_checker_tests
	$(BIN_DIR)/mqtt_client_tests
	$(BIN_DIR)/mqtt_mapping_tests

e2e-corpus: $(BIN_DIR)/e2e_machine_sequences $(BIN_DIR)/e2e_machine_domains $(BIN_DIR)/e2e_domain_scenarios $(BIN_DIR)/e2e_ai_trigger_dispatch $(BIN_DIR)/e2e_yuma_localai_cascade $(BIN_DIR)/cesgen_oracles_parity $(BIN_DIR)/cesgen_provenance $(BIN_DIR)/cesgen_composition $(BIN_DIR)/cesgen_governance $(BIN_DIR)/cesgen_contracts_parity $(BIN_DIR)/cesgen_deprecation
	$(BIN_DIR)/e2e_machine_sequences ../RealityEngine_Machines/machines
	$(BIN_DIR)/e2e_machine_domains ../RealityEngine_Machines/machines
	$(BIN_DIR)/e2e_domain_scenarios ../RealityEngine_Machines/machines
	$(BIN_DIR)/e2e_ai_trigger_dispatch ../RealityEngine_Machines/machines
	$(BIN_DIR)/e2e_yuma_localai_cascade ../RealityEngine_Machines/machines
	$(BIN_DIR)/cesgen_oracles_parity ../RealityEngine_Machines/oracles.json ../RealityEngine_Machines/machines
	$(BIN_DIR)/cesgen_provenance ../RealityEngine_Machines/machines
	$(BIN_DIR)/cesgen_composition ../RealityEngine_Machines/machines
	$(BIN_DIR)/cesgen_governance ../RealityEngine_Machines/machines
	$(BIN_DIR)/cesgen_contracts_parity ../RealityEngine_Machines/contracts.json ../RealityEngine_Machines/machines
	$(BIN_DIR)/cesgen_deprecation ../RealityEngine_Machines/machines

e2e-services: $(BIN_DIR)/reality_engine_server $(BIN_DIR)/perception_engine_server $(BIN_DIR)/reality_engine_cli
	tests/e2e_services.sh

e2e-healthkit-spezi: $(BIN_DIR)/reality_engine_server $(BIN_DIR)/perception_engine_server
	tests/e2e_healthkit_spezi.sh

e2e: e2e-corpus e2e-services e2e-healthkit-spezi

clean:
	rm -rf $(BIN_DIR) $(OBJ_DIR)

# Generated .d files record each object's header dependencies. Included last,
# and with `-` so a clean tree (no .d files yet) is not an error.
-include $(DEPS)
