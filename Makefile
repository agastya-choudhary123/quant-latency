# Tick-to-order execution engine. macOS/clang, C++20.
#   OpenSSL : Homebrew openssl@3 (TLS WebSocket transport)
#   libcurl : from the macOS SDK (REST connector, used by tests)
CXX      ?= clang++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Icpp/include
OPENSSL  ?= $(shell brew --prefix openssl@3 2>/dev/null)
SSL_INC   = -I$(OPENSSL)/include
SSL_LIB   = -L$(OPENSSL)/lib -lssl -lcrypto
LDFLAGS  ?= -lcurl
BIN       = bin
SRC       = cpp/src

.PHONY: all test test_live clean

all: $(BIN)/tick2order $(BIN)/capture $(BIN)/tests_infra

$(BIN):
	mkdir -p $(BIN)

# Capture: the ONLY networked tool. Records real frames once for replay.
$(BIN)/capture: $(SRC)/capture_main.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(SSL_INC) $< -o $@ $(SSL_LIB)

# THE headline binary: tick-to-order execution latency, full path.
$(BIN)/tick2order: $(SRC)/tick2order_main.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(SSL_INC) $< -o $@ $(SSL_LIB)

$(BIN)/tests_infra: cpp/tests/test_infra.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(SSL_INC) $< -o $@ $(SSL_LIB)

# Infrastructure tests: SPSC ring, clock, JSON parsing, risk, venue routing.
test: $(BIN)/tests_infra
	./$(BIN)/tests_infra

# Live integration smoke test — requires network egress to the exchanges.
test_live: $(BIN)/tests_infra
	./$(BIN)/tests_infra --live

clean:
	rm -rf $(BIN)
