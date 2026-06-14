CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -Isrc
LDFLAGS ?=
PREFIX ?= /usr/local

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN := pkchat
TEST_BIN := $(BUILD_DIR)/test_runner

SRC := $(shell find src -name '*.cpp' | sort)
APP_SRC := $(SRC)
LIB_SRC := $(filter-out src/main.cpp,$(SRC))
APP_OBJ := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(APP_SRC))
LIB_OBJ := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(LIB_SRC))
TEST_SRC := $(shell find tests/unit -name '*.cpp' | sort)
TEST_OBJ := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(TEST_SRC))

.PHONY: all clean test test-unit test-integration sanitize test-sanitize leak-check test-leak install

all: $(BIN)

$(BIN): $(APP_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_BIN): $(LIB_OBJ) $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: test-unit test-integration

test-unit: $(TEST_BIN)
	$(TEST_BIN)

test-integration: $(BIN)
	tests/integration/test_mock_server.sh

sanitize:
	$(MAKE) clean
	$(MAKE) CXXFLAGS="$(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined" all

test-sanitize:
	$(MAKE) clean
	$(MAKE) CXXFLAGS="$(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined" test

leak-check: $(BIN)
	@if command -v valgrind >/dev/null 2>&1; then \
		valgrind --leak-check=full --error-exitcode=1 ./$(BIN) --version >/dev/null; \
	else \
		echo "valgrind not found; running sanitizer build smoke test instead"; \
		$(MAKE) test-sanitize; \
	fi

test-leak: leak-check

install: $(BIN)
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 0755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/$(BIN)"

clean:
	rm -rf $(BUILD_DIR) $(BIN)
