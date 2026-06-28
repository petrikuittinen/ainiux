CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -Isrc
LDFLAGS ?=
LIBCURL_CFLAGS ?= $(shell pkg-config --cflags libcurl 2>/dev/null || curl-config --cflags 2>/dev/null)
LIBCURL_LIBS ?= $(shell pkg-config --libs libcurl 2>/dev/null || curl-config --libs 2>/dev/null)
SQLITE_CFLAGS ?= $(shell pkg-config --cflags sqlite3 2>/dev/null)
SQLITE_LIBS ?= $(shell pkg-config --libs sqlite3 2>/dev/null || printf '%s\n' -lsqlite3)
CXXFLAGS += $(LIBCURL_CFLAGS) $(SQLITE_CFLAGS)
LDFLAGS += $(LIBCURL_LIBS) $(SQLITE_LIBS)
PREFIX ?= /usr/local
SYSCONFDIR ?= /etc
BUILD_DIR := build
GENERATED_DIR := $(BUILD_DIR)/generated
CXXFLAGS += -I$(GENERATED_DIR)
DEBUG_FLAG_PATTERNS := -g -g0 -g1 -g2 -g3 -ggdb -ggdb% -glldb -glldb% -gdwarf% -gstabs%
OPTIMIZED_CXXFLAGS := $(filter-out -O% $(DEBUG_FLAG_PATTERNS),$(CXXFLAGS)) -O3 -DNDEBUG
OPTIMIZED_LDFLAGS := $(LDFLAGS) -s

OBJ_DIR := $(BUILD_DIR)/obj
BIN := pkchat
TEST_BIN := $(BUILD_DIR)/test_runner
COMMON_CONFIG := config/pkchat.conf
COMMON_CONFIG_DIR := $(DESTDIR)$(SYSCONFDIR)/xdg/pkchat
COMMON_CONFIG_PATH := $(COMMON_CONFIG_DIR)/config.conf
BENCHMARK_DATA_DIR := $(DESTDIR)$(PREFIX)/share/pkchat/benchmarks
BUILTIN_BENCHMARK_HEADER := $(GENERATED_DIR)/builtin_dataset.hpp

SRC := $(shell find src -name '*.cpp' | sort)
APP_SRC := $(SRC)
LIB_SRC := $(filter-out src/main.cpp,$(SRC))
APP_OBJ := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(APP_SRC))
LIB_OBJ := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(LIB_SRC))
TEST_SRC := $(shell find tests/unit -name '*.cpp' | sort)
TEST_OBJ := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(TEST_SRC))
DEP := $(sort $(APP_OBJ:.o=.d) $(TEST_OBJ:.o=.d))

.PHONY: all clean optimized test test-unit test-integration sanitize test-sanitize leak-check test-leak install

all: $(BIN)

$(BIN): $(APP_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_BIN): $(LIB_OBJ) $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILTIN_BENCHMARK_HEADER): benchmarks/builtin.jsonl
	@mkdir -p $(dir $@)
	@{ \
		printf '%s\n' '#pragma once' 'namespace pkchat::benchmark {' \
			'inline constexpr char kBuiltinDatasetJsonl[] = R"PKCHAT_JSONL('; \
		cat $<; \
		printf '%s\n' ')PKCHAT_JSONL";' '}  // namespace pkchat::benchmark'; \
	} >$@.tmp
	@mv $@.tmp $@

$(OBJ_DIR)/src/benchmark/benchmark.o: $(BUILTIN_BENCHMARK_HEADER)

-include $(DEP)

test: test-unit test-integration

test-unit: $(TEST_BIN)
	$(TEST_BIN)

test-integration: $(BIN)
	tests/integration/test_mock_server.sh

optimized:
	$(MAKE) clean
	$(MAKE) CXXFLAGS="$(OPTIMIZED_CXXFLAGS)" LDFLAGS="$(OPTIMIZED_LDFLAGS)" all

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

install: $(BIN) $(COMMON_CONFIG)
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 0755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	install -d "$(COMMON_CONFIG_DIR)"
	@if test -e "$(COMMON_CONFIG_PATH)"; then \
		echo "Preserving existing system config: $(COMMON_CONFIG_PATH)"; \
	else \
		install -m 0644 "$(COMMON_CONFIG)" "$(COMMON_CONFIG_PATH)"; \
	fi
	install -d "$(BENCHMARK_DATA_DIR)"
	install -m 0644 benchmarks/builtin.jsonl benchmarks/long-context.jsonl "$(BENCHMARK_DATA_DIR)"

clean:
	rm -rf $(BUILD_DIR) $(BIN)
