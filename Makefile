# Freshness Lab — build, test, sanitize, matrix. HOST SIMULATION only.
CC      ?= gcc
BUILD   ?= build
OPT     ?= -O2
SAN     ?=
PYTHON  ?= python3

WARN := -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow \
        -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wundef -Wdouble-promotion \
        -Wformat=2 -Wvla -Werror
CFLAGS  += $(WARN) $(OPT) $(SAN) $(EXTRA_CFLAGS)
LDFLAGS += $(SAN)

CORE_SRC := $(wildcard core/*.c)
HOST_SRC := host/aoi.c host/sim.c host/csvio.c
CORE_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRC))
HOST_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(HOST_SRC))
TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRC))

.PHONY: all test sanitize clang-check matrix clean tools-test check

all: $(BUILD)/flsim

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/flsim: $(CORE_OBJ) $(HOST_OBJ) $(BUILD)/host/flsim_main.o
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/test_%: tests/test_%.c $(CORE_OBJ) $(HOST_OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< $(CORE_OBJ) $(HOST_OBJ) $(LDFLAGS) -o $@

test: $(TEST_BIN) $(BUILD)/flsim
	@fail=0; for t in $(TEST_BIN); do echo "== $$t"; $$t || fail=1; done; \
	 if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; echo "ALL C TESTS PASSED"

# Address + undefined-behaviour sanitizers (gcc). Separate build dir.
sanitize:
	$(MAKE) BUILD=build-san OPT=-O1 SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" test

# Compile the core with clang -Weverything as a second opinion (no link).
clang-check:
	@for f in $(CORE_SRC) $(HOST_SRC) host/flsim_main.c; do \
	  clang $(WARN) -Weverything -Wno-padded -Wno-declaration-after-statement -Wno-unsafe-buffer-usage \
	    -Wno-switch-default -Wno-covered-switch-default -Wno-reserved-macro-identifier \
	    -Wno-reserved-identifier -Wno-missing-noreturn -O2 -c $$f -o /dev/null || exit 1; done; echo "clang-check ok"

tools-test:
	$(PYTHON) -m unittest discover -s tests -p 'test_*.py' -v

check: test tools-test

matrix: $(BUILD)/flsim
	$(PYTHON) tools/run_matrix.py --flsim $(BUILD)/flsim --out results/matrix

clean:
	rm -rf $(BUILD) build-san
