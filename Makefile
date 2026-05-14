CXX ?= g++
BOOST_PREFIX ?= $(shell brew --prefix boost 2>/dev/null)
BOOST_CPPFLAGS ?= $(if $(BOOST_PREFIX),-I$(BOOST_PREFIX)/include,)
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -pedantic -Iinclude $(BOOST_CPPFLAGS)
LDFLAGS ?= -pthread

BIN_DIR := bin
SRC := src/reality.cpp src/http.cpp

.PHONY: all clean test e2e e2e-corpus e2e-services

all: $(BIN_DIR)/reality_engine_server $(BIN_DIR)/perception_engine_server $(BIN_DIR)/reality_engine_tests $(BIN_DIR)/e2e_machine_sequences $(BIN_DIR)/e2e_machine_domains $(BIN_DIR)/e2e_domain_scenarios $(BIN_DIR)/cesgen_index_compile $(BIN_DIR)/cesgen_oracles_parity $(BIN_DIR)/cesgen_provenance $(BIN_DIR)/cesgen_composition $(BIN_DIR)/cesgen_governance $(BIN_DIR)/cesgen_contracts_parity $(BIN_DIR)/cesgen_deprecation

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/reality_engine_server: $(SRC) src/reality_engine_server.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SRC) src/reality_engine_server.cpp -o $@ $(LDFLAGS)

$(BIN_DIR)/perception_engine_server: $(SRC) src/perception_engine_server.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SRC) src/perception_engine_server.cpp -o $@ $(LDFLAGS)

$(BIN_DIR)/reality_engine_tests: $(SRC) tests/reality_engine_tests.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SRC) tests/reality_engine_tests.cpp -o $@ $(LDFLAGS)

$(BIN_DIR)/e2e_machine_sequences: $(SRC) tests/e2e_machine_sequences.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SRC) tests/e2e_machine_sequences.cpp -o $@ $(LDFLAGS)

$(BIN_DIR)/e2e_machine_domains: $(SRC) tests/e2e_machine_domains.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SRC) tests/e2e_machine_domains.cpp -o $@ $(LDFLAGS)

$(BIN_DIR)/e2e_domain_scenarios: $(SRC) tests/e2e_domain_scenarios.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SRC) tests/e2e_domain_scenarios.cpp -o $@ $(LDFLAGS)

$(BIN_DIR)/cesgen_index_compile: tests/cesgen_index_compile.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) tests/cesgen_index_compile.cpp -o $@

$(BIN_DIR)/cesgen_oracles_parity: $(SRC) tests/cesgen_oracles_parity.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SRC) tests/cesgen_oracles_parity.cpp -o $@ $(LDFLAGS)

$(BIN_DIR)/cesgen_provenance: $(SRC) tests/cesgen_provenance.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SRC) tests/cesgen_provenance.cpp -o $@ $(LDFLAGS)

$(BIN_DIR)/cesgen_composition: $(SRC) tests/cesgen_composition.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SRC) tests/cesgen_composition.cpp -o $@ $(LDFLAGS)

$(BIN_DIR)/cesgen_governance: $(SRC) tests/cesgen_governance.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SRC) tests/cesgen_governance.cpp -o $@ $(LDFLAGS)

$(BIN_DIR)/cesgen_contracts_parity: $(SRC) tests/cesgen_contracts_parity.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SRC) tests/cesgen_contracts_parity.cpp -o $@ $(LDFLAGS)

$(BIN_DIR)/cesgen_deprecation: $(SRC) tests/cesgen_deprecation.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SRC) tests/cesgen_deprecation.cpp -o $@ $(LDFLAGS)

test: $(BIN_DIR)/reality_engine_tests
	$(BIN_DIR)/reality_engine_tests

e2e-corpus: $(BIN_DIR)/e2e_machine_sequences $(BIN_DIR)/e2e_machine_domains $(BIN_DIR)/e2e_domain_scenarios $(BIN_DIR)/cesgen_oracles_parity $(BIN_DIR)/cesgen_provenance $(BIN_DIR)/cesgen_composition $(BIN_DIR)/cesgen_governance $(BIN_DIR)/cesgen_contracts_parity
	$(BIN_DIR)/e2e_machine_sequences ../RealityEngine_AI/examples/machines
	$(BIN_DIR)/e2e_machine_domains ../RealityEngine_AI/examples/machines
	$(BIN_DIR)/e2e_domain_scenarios ../RealityEngine_AI/examples/machines
	$(BIN_DIR)/cesgen_oracles_parity ../RealityEngine_AI/examples/oracles.json ../RealityEngine_AI/examples/machines
	$(BIN_DIR)/cesgen_provenance ../RealityEngine_AI/examples/machines
	$(BIN_DIR)/cesgen_composition ../RealityEngine_AI/examples/machines
	$(BIN_DIR)/cesgen_governance ../RealityEngine_AI/examples/machines
	$(BIN_DIR)/cesgen_contracts_parity ../RealityEngine_AI/examples/contracts.json ../RealityEngine_AI/examples/machines
	$(BIN_DIR)/cesgen_deprecation ../RealityEngine_AI/examples/machines

e2e-services: $(BIN_DIR)/reality_engine_server $(BIN_DIR)/perception_engine_server
	tests/e2e_services.sh

e2e: e2e-corpus e2e-services

clean:
	rm -rf $(BIN_DIR)
