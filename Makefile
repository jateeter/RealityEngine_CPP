CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -pedantic -Iinclude
LDFLAGS ?= -pthread

BIN_DIR := bin
SRC := src/reality.cpp src/http.cpp

.PHONY: all clean test

all: $(BIN_DIR)/reality_engine_server $(BIN_DIR)/perception_engine_server $(BIN_DIR)/reality_engine_tests $(BIN_DIR)/e2e_machine_sequences

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

test: $(BIN_DIR)/reality_engine_tests
	$(BIN_DIR)/reality_engine_tests

e2e: $(BIN_DIR)/e2e_machine_sequences
	$(BIN_DIR)/e2e_machine_sequences ../RealityEngine_AI/examples/machines

clean:
	rm -rf $(BIN_DIR)
