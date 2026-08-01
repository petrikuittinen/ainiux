CXX ?= g++
DEFAULT_JOBS ?= 10
# Use a conservative parallel default on this 20-core development machine.
# A command-line -j/--jobs remains authoritative, and recursive $(MAKE)
# invocations inherit the same GNU make jobserver.
ifeq ($(MAKELEVEL),0)
ifeq ($(filter -j% --jobs=%,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(DEFAULT_JOBS)
endif
endif

CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -Isrc -Itests/unit
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
BIN := ainiux
TEST_BIN := $(BUILD_DIR)/test_runner
IO_FAULT_BIN := $(BUILD_DIR)/test_io_faults
POSIX_IO_MOCK := $(BUILD_DIR)/posix_io_mock.so
IO_FAULT_OBJ := $(OBJ_DIR)/tests/unit/test_io_faults.o \
                $(OBJ_DIR)/tests/unit/io/test_io_faults.o \
                $(OBJ_DIR)/tests/unit/mock/slow_server.o \
                $(OBJ_DIR)/tests/unit/http/test_http_network.o \
                $(OBJ_DIR)/tests/unit/support/test_support.o
COMMON_CONFIG := config/ainiux.conf
EDITOR_COMMANDS_CONFIG := config/editor-commands.conf
THEMES_CONFIG := config/themes.conf
BENCHMARKS_CONFIG := config/benchmarks.conf
MODELS_CONFIG := config/models.conf
CONFIG_MIGRATOR := scripts/migrate-config-booleans.sh
MODELS_CONFIG_HEADER := $(GENERATED_DIR)/embedded_models_config.hpp
EDITOR_COMMANDS_CONFIG_HEADER := $(GENERATED_DIR)/embedded_editor_commands.hpp
COMMON_CONFIG_DIR := $(DESTDIR)$(SYSCONFDIR)/xdg/ainiux
COMMON_CONFIG_PATH := $(COMMON_CONFIG_DIR)/config.conf
EDITOR_COMMANDS_CONFIG_PATH := $(COMMON_CONFIG_DIR)/editor-commands.conf
THEMES_CONFIG_PATH := $(COMMON_CONFIG_DIR)/themes.conf
BENCHMARKS_CONFIG_PATH := $(COMMON_CONFIG_DIR)/benchmarks.conf
MODELS_CONFIG_PATH := $(COMMON_CONFIG_DIR)/models.conf
EDITOR_COMMANDS_INSTALL := $(DESTDIR)$(PREFIX)/share/ainiux/editor-commands.conf
THEMES_INSTALL := $(DESTDIR)$(PREFIX)/share/ainiux/themes.conf
BENCHMARKS_INSTALL := $(DESTDIR)$(PREFIX)/share/ainiux/benchmarks.conf
MODELS_INSTALL := $(DESTDIR)$(PREFIX)/share/ainiux/models.conf
BENCHMARK_DATA_DIR := $(DESTDIR)$(PREFIX)/share/ainiux/benchmarks
BUILTIN_BENCHMARK_HEADER := $(GENERATED_DIR)/builtin_dataset.hpp
EDITOR_HELP_SRC := docs/editor_help.md
EDITOR_HELP_HEADER := $(GENERATED_DIR)/embedded_editor_help.hpp
EDITOR_HELP_INSTALL := $(DESTDIR)$(PREFIX)/share/ainiux/editor_help.md
MASTER_PROMPT_SRC := resources/prompts/master_prompt.md
SECURITY_PROMPT_SRC := resources/prompts/security_prompt.md
AGENT_PROMPT_SRC := resources/prompts/agent_prompt.md
AGENT_PROMPTS_HEADER := $(GENERATED_DIR)/embedded_agent_prompts.hpp
AGENT_PROMPTS_INSTALL_DIR := $(DESTDIR)$(PREFIX)/share/ainiux/prompts
BUILTIN_DATASET_PARTS := benchmarks/builtin/safety.jsonl \
                         benchmarks/builtin/reasoning.jsonl \
                         benchmarks/builtin/writing.jsonl \
                         benchmarks/builtin/coding.jsonl \
                         benchmarks/builtin/multi-turn.jsonl \
                         benchmarks/builtin/cutoff.jsonl
BUILTIN_DATASET := benchmarks/builtin.jsonl

SRC := $(shell find src -name '*.cpp' | sort)
APP_SRC := $(SRC)
LIB_SRC := $(filter-out src/main.cpp,$(SRC))
APP_OBJ := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(APP_SRC))
LIB_OBJ := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(LIB_SRC))
TEST_SRC := $(filter-out tests/unit/test_io_faults.cpp tests/unit/io/test_io_faults.cpp tests/unit/mock/slow_server.cpp tests/unit/http/test_http_network.cpp,$(shell find tests/unit -name '*.cpp' | sort))
TEST_OBJ := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(TEST_SRC))
IO_FAULT_DEP := $(IO_FAULT_OBJ:.o=.d)
DEP := $(sort $(APP_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(IO_FAULT_DEP))

VALGRIND ?= valgrind --error-exitcode=1 --leak-check=full --show-leak-kinds=definite,indirect --quiet

.PHONY: all clean optimized test test-full test-unit test-unit-faults test-integration-smoke test-integration test-integration-sqlite sanitize test-sanitize leak-check test-leak install

all: $(BIN)

$(BIN): $(APP_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_BIN): $(LIB_OBJ) $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(IO_FAULT_BIN): $(LIB_OBJ) $(IO_FAULT_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(POSIX_IO_MOCK): tests/mock/posix_io_mock.c
	@mkdir -p $(dir $@)
	$(CC) -shared -fPIC -Wall -Wextra -Wpedantic -o $@ $<

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILTIN_DATASET): $(BUILTIN_DATASET_PARTS)
	@cat $(BUILTIN_DATASET_PARTS) >$@.tmp
	@mv $@.tmp $@

$(BUILTIN_BENCHMARK_HEADER): $(BUILTIN_DATASET)
	@mkdir -p $(dir $@)
	@{ \
		printf '%s\n' '#pragma once' 'namespace ainiux::benchmark {' \
			'inline constexpr char kBuiltinDatasetJsonl[] = R"AINIUX_JSONL('; \
		cat $<; \
		printf '%s\n' ')AINIUX_JSONL";' '}  // namespace ainiux::benchmark'; \
	} >$@.tmp
	@mv $@.tmp $@

$(OBJ_DIR)/src/benchmark/dataset.o: $(BUILTIN_BENCHMARK_HEADER)

$(MODELS_CONFIG_HEADER): $(MODELS_CONFIG)
	@mkdir -p $(dir $@)
	@{ \
		printf '%s\n' '#pragma once' 'namespace ainiux::config {' \
			'inline constexpr char kEmbeddedModelsConfig[] = R"AINIUX_MODELS('; \
		cat $<; \
		printf '%s\n' ')AINIUX_MODELS";' '}  // namespace ainiux::config'; \
	} >$@.tmp
	@mv $@.tmp $@

$(EDITOR_COMMANDS_CONFIG_HEADER): $(EDITOR_COMMANDS_CONFIG)
	@mkdir -p $(dir $@)
	@{ \
		printf '%s\n' '#pragma once' 'namespace ainiux::config {' \
			'inline constexpr char kEmbeddedEditorCommandsConfig[] = R"AINIUX_EDCMD('; \
		cat $<; \
		printf '%s\n' ')AINIUX_EDCMD";' '}  // namespace ainiux::config'; \
	} >$@.tmp
	@mv $@.tmp $@

$(OBJ_DIR)/src/config/config.o: $(MODELS_CONFIG_HEADER) $(EDITOR_COMMANDS_CONFIG_HEADER)

$(EDITOR_HELP_HEADER): $(EDITOR_HELP_SRC)
	@mkdir -p $(dir $@)
	@{ \
		printf '%s\n' '#pragma once' 'namespace ainiux::editor {' \
			'inline constexpr char kEditorHelpMarkdown[] = R"AINIUX_HELP('; \
		cat $<; \
		printf '%s\n' ')AINIUX_HELP";' '}  // namespace ainiux::editor'; \
	} >$@.tmp
	@mv $@.tmp $@

$(OBJ_DIR)/src/editor/editor_help.o: $(EDITOR_HELP_HEADER)

$(AGENT_PROMPTS_HEADER): $(MASTER_PROMPT_SRC) $(SECURITY_PROMPT_SRC) $(AGENT_PROMPT_SRC)
	@mkdir -p $(dir $@)
	@{ \
		printf '%s\n' '#pragma once' 'namespace ainiux::agent {' \
			'inline constexpr char kEmbeddedMasterPrompt[] = R"AINIUX_MASTER('; \
		cat $(MASTER_PROMPT_SRC); \
		printf '%s\n' ')AINIUX_MASTER";' \
			'inline constexpr char kEmbeddedSecurityPrompt[] = R"AINIUX_SECURITY('; \
		cat $(SECURITY_PROMPT_SRC); \
		printf '%s\n' ')AINIUX_SECURITY";' \
			'inline constexpr char kEmbeddedAgentPrompt[] = R"AINIUX_AGENT('; \
		cat $(AGENT_PROMPT_SRC); \
		printf '%s\n' ')AINIUX_AGENT";' '}  // namespace ainiux::agent'; \
	} >$@.tmp
	@mv $@.tmp $@

$(OBJ_DIR)/src/agent/prompts.o: $(AGENT_PROMPTS_HEADER)

-include $(DEP)

test:
	$(MAKE) test-unit
	$(MAKE) test-integration-smoke

test-full:
	$(MAKE) test-unit
	$(MAKE) test-unit-faults
	$(MAKE) test-integration

test-unit: $(TEST_BIN)
	$(TEST_BIN)
	tests/unit/config/test_config_migration.sh

test-unit-faults: $(IO_FAULT_BIN) $(POSIX_IO_MOCK)
	$(IO_FAULT_BIN)
	tools/run_enospc_test.sh "$(CXX)" "$(abspath $(POSIX_IO_MOCK))" "$(abspath $(IO_FAULT_BIN))"

test-integration-smoke: $(BIN)
	tests/integration/test_mock_smoke.sh

test-integration: $(BIN)
	tests/integration/test_code_index.sh
	tests/integration/test_mock_server.sh
	sh tests/integration/test_llama_server.sh

test-integration-sqlite: $(BIN)
	tests/integration/test_sqlite_persistence.sh

optimized:
	$(MAKE) clean
	$(MAKE) CXXFLAGS="$(OPTIMIZED_CXXFLAGS)" LDFLAGS="$(OPTIMIZED_LDFLAGS)" all

sanitize:
	$(MAKE) clean
	$(MAKE) CXXFLAGS="$(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined" all

test-sanitize:
	$(MAKE) clean
	$(MAKE) CXXFLAGS="$(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined" test-full

leak-check: $(BIN) $(TEST_BIN) $(IO_FAULT_BIN)
	@if command -v valgrind >/dev/null 2>&1; then \
		$(VALGRIND) ./$(TEST_BIN); \
		$(VALGRIND) ./$(IO_FAULT_BIN); \
		$(VALGRIND) ./$(BIN) --version >/dev/null; \
	else \
		echo "valgrind not found; running sanitizer build smoke test instead"; \
		$(MAKE) test-sanitize; \
	fi

test-leak: leak-check

install: $(BIN) $(COMMON_CONFIG) $(EDITOR_COMMANDS_CONFIG) $(THEMES_CONFIG) $(BENCHMARKS_CONFIG) $(MODELS_CONFIG) $(CONFIG_MIGRATOR) $(EDITOR_HELP_SRC) $(MASTER_PROMPT_SRC) $(SECURITY_PROMPT_SRC) $(AGENT_PROMPT_SRC)
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 0755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	install -d "$(COMMON_CONFIG_DIR)"
	@if test -e "$(COMMON_CONFIG_PATH)"; then \
		"$(CONFIG_MIGRATOR)" "$(COMMON_CONFIG_PATH)"; \
		echo "Preserving existing system config: $(COMMON_CONFIG_PATH)"; \
	else \
		install -m 0644 "$(COMMON_CONFIG)" "$(COMMON_CONFIG_PATH)"; \
	fi
	@if test -e "$(EDITOR_COMMANDS_CONFIG_PATH)"; then \
		echo "Preserving existing system editor commands: $(EDITOR_COMMANDS_CONFIG_PATH)"; \
	else \
		install -m 0644 "$(EDITOR_COMMANDS_CONFIG)" "$(EDITOR_COMMANDS_CONFIG_PATH)"; \
	fi
	@if test -e "$(THEMES_CONFIG_PATH)"; then \
		echo "Preserving existing system themes: $(THEMES_CONFIG_PATH)"; \
	else \
		install -m 0644 "$(THEMES_CONFIG)" "$(THEMES_CONFIG_PATH)"; \
	fi
	@if test -e "$(BENCHMARKS_CONFIG_PATH)"; then \
		echo "Preserving existing system benchmark prompts: $(BENCHMARKS_CONFIG_PATH)"; \
	else \
		install -m 0644 "$(BENCHMARKS_CONFIG)" "$(BENCHMARKS_CONFIG_PATH)"; \
	fi
	@if test -e "$(MODELS_CONFIG_PATH)"; then \
		"$(CONFIG_MIGRATOR)" "$(MODELS_CONFIG_PATH)"; \
		echo "Preserving existing system model catalog: $(MODELS_CONFIG_PATH)"; \
	else \
		install -m 0644 "$(MODELS_CONFIG)" "$(MODELS_CONFIG_PATH)"; \
	fi
	install -d "$(BENCHMARK_DATA_DIR)"
	install -m 0644 $(BUILTIN_DATASET) benchmarks/long-context.jsonl $(BUILTIN_DATASET_PARTS) "$(BENCHMARK_DATA_DIR)"
	install -d "$(DESTDIR)$(PREFIX)/share/ainiux"
	install -m 0644 "$(EDITOR_HELP_SRC)" "$(EDITOR_HELP_INSTALL)"
	install -m 0644 "$(EDITOR_COMMANDS_CONFIG)" "$(EDITOR_COMMANDS_INSTALL)"
	install -m 0644 "$(THEMES_CONFIG)" "$(THEMES_INSTALL)"
	install -m 0644 "$(BENCHMARKS_CONFIG)" "$(BENCHMARKS_INSTALL)"
	install -m 0644 "$(MODELS_CONFIG)" "$(MODELS_INSTALL)"
	install -d "$(AGENT_PROMPTS_INSTALL_DIR)"
	install -m 0644 "$(MASTER_PROMPT_SRC)" "$(SECURITY_PROMPT_SRC)" "$(AGENT_PROMPT_SRC)" \
		"$(AGENT_PROMPTS_INSTALL_DIR)"

clean:
	rm -rf $(BUILD_DIR) $(BIN) $(IO_FAULT_BIN)
