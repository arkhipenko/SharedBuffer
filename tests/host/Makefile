# Host unit tests for the SharedBuffer library
# Usage: make -C tests/host [CXX=clang++] [STD=gnu++11|c++17|c++20]
#
# Variants of test_sharedbuffer.cpp (all with STD):
#   default     AddressSanitizer and UndefinedBehaviorSanitizer, built-in host lock
#   align1      SHARED_BUFFER_ALIGN=1 (the AVR default)
#   align32     SHARED_BUFFER_ALIGN=32 and SHARED_BUFFER_MAX_BUFFERS=127
#   counted     critical section replaced by counting hooks (cs_hooks.h): every enter
#               has an exit, sections never nest, the dump writer runs outside
#   o2          -O2, no sanitizers
#   tsan        ThreadSanitizer, for the thread test
# Every variant links early_user.cpp before SharedBuffer.cpp: a global constructor that
# registers a buffer in SharedBuf during static initialization.
# Also checked: bad SHARED_BUFFER_MAX_BUFFERS and SHARED_BUFFER_ALIGN stop the build,
# RP2040 and RP2350 builds stop without the override macros, SharedBuffer.cpp emits no
# global constructor, and the version agrees in the header, library.json,
# library.properties and README.md.

STD  ?= gnu++11
OUT  = _build
SRC  = ../../src
LIB  = $(SRC)/SharedBuffer.cpp $(SRC)/SharedBuffer.h
TST  = test_sharedbuffer.cpp early_user.cpp cs_hooks.h
WARN = -Wall -Wextra -Wpedantic -Wshadow -Werror
SAN  = -fsanitize=address,undefined -fno-sanitize-recover=all
BASE = -std=$(STD) -g $(WARN) -I$(SRC) -pthread

# early_user.cpp comes before SharedBuffer.cpp on purpose (see early_user.cpp)
SRCS = test_sharedbuffer.cpp early_user.cpp $(SRC)/SharedBuffer.cpp

BINS = $(OUT)/default $(OUT)/align1 $(OUT)/align32 $(OUT)/counted $(OUT)/o2

# begin() is tested with a request malloc() cannot satisfy
export ASAN_OPTIONS = allocator_may_return_null=1
export TSAN_OPTIONS = allocator_may_return_null=1

test: version config_check platform_check static_init_check $(BINS) $(OUT)/tsan
	@for t in $(BINS); do echo "== $$t ($(CXX), $(STD))"; ./$$t || exit 1; done
	@echo "== $(OUT)/tsan ($(CXX), $(STD))"
	@setarch $$(uname -m) -R ./$(OUT)/tsan   # ASLR off: TSan fails on some kernels' mmap layouts
	@echo "HOST TESTS PASSED"

$(OUT):
	mkdir -p $(OUT)

$(OUT)/default: $(TST) $(LIB) | $(OUT)
	$(CXX) -O1 $(BASE) $(SAN) $(SRCS) -o $@

$(OUT)/align1: $(TST) $(LIB) | $(OUT)
	$(CXX) -O1 $(BASE) $(SAN) -DSHARED_BUFFER_ALIGN=1 $(SRCS) -o $@

$(OUT)/align32: $(TST) $(LIB) | $(OUT)
	$(CXX) -O1 $(BASE) $(SAN) -DSHARED_BUFFER_ALIGN=32 -DSHARED_BUFFER_MAX_BUFFERS=127 $(SRCS) -o $@

$(OUT)/counted: $(TST) $(LIB) | $(OUT)
	$(CXX) -O1 $(BASE) $(SAN) -DTEST_COUNTED_CRITICAL -include cs_hooks.h $(SRCS) -o $@

$(OUT)/o2: $(TST) $(LIB) | $(OUT)
	$(CXX) -O2 $(BASE) $(SRCS) -o $@

$(OUT)/tsan: $(TST) $(LIB) | $(OUT)
	$(CXX) -O1 $(BASE) -fsanitize=thread $(SRCS) -o $@

config_check:
	@for d in "SHARED_BUFFER_MAX_BUFFERS=0" "SHARED_BUFFER_MAX_BUFFERS=128" "SHARED_BUFFER_ALIGN=0" "SHARED_BUFFER_ALIGN=3" "SHARED_BUFFER_ALIGN=12"; do \
	     if echo '#include "SharedBuffer.h"' | $(CXX) -std=$(STD) -x c++ -fsyntax-only -I$(SRC) -D$$d - 2>/dev/null; then \
	         echo "FAIL config: $$d was accepted"; exit 1; fi; done
	@echo "== config (bad SHARED_BUFFER_MAX_BUFFERS and SHARED_BUFFER_ALIGN stop the build)"

# Two-core RP2040 and RP2350 builds must stop unless the build supplies its own lock
platform_check:
	@for d in "PICO_RP2040=1" "PICO_RP2350=1" "ARDUINO_ARCH_RP2040" "ARDUINO_ARCH_MBED_RP2040"; do \
	     if $(CXX) -std=$(STD) -fsyntax-only -I$(SRC) -D$$d $(SRC)/SharedBuffer.cpp 2>/dev/null; then \
	         echo "FAIL platform: $$d was accepted"; exit 1; fi; \
	     if ! $(CXX) -std=$(STD) -fsyntax-only -I$(SRC) -D$$d -include cs_hooks.h $(SRC)/SharedBuffer.cpp; then \
	         echo "FAIL platform: $$d with the override macros was refused"; exit 1; fi; done
	@echo "== platform (RP2040 and RP2350 stop the build unless the override macros are defined)"

static_init_check: | $(OUT)
	@$(CXX) -std=$(STD) -O1 $(WARN) -I$(SRC) -c $(SRC)/SharedBuffer.cpp -o $(OUT)/SharedBuffer.o
	@if nm $(OUT)/SharedBuffer.o | grep -q "_GLOBAL__sub_I"; then \
	     echo "FAIL static init: SharedBuffer.cpp has a global constructor"; exit 1; fi
	@echo "== static init (SharedBuffer.cpp has no global constructor)"

version:
	@j=$$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' ../../library.json); \
	 p=$$(sed -n 's/^version=//p' ../../library.properties | tr -d '\r'); \
	 h=$$(sed -n 's/.*SHARED_BUFFER_VERSION_STRING *"\([^"]*\)".*/\1/p' $(SRC)/SharedBuffer.h); \
	 n=$$(sed -n 's/.*define SHARED_BUFFER_VERSION  *\([0-9][0-9]*\).*/\1/p' $(SRC)/SharedBuffer.h); \
	 r=$$(sed -n 's/^Version: *\([0-9.]*\).*/\1/p' ../../README.md | tr -d '\r'); \
	 hist=$$(grep -c "^- $$h (" ../../README.md); \
	 num=$$(echo "$$h" | awk -F. '{ print $$1 * 10000 + $$2 * 100 + $$3 }'); \
	 if [ -z "$$h" ] || [ "$$j" != "$$h" ] || [ "$$p" != "$$h" ] || [ "$$r" != "$$h" ] || [ "$$n" != "$$num" ] || [ "$$hist" != "1" ]; then \
	     echo "FAIL version: SharedBuffer.h '$$h' ($$n), library.json '$$j', library.properties '$$p', README '$$r', README history entries $$hist"; exit 1; fi; \
	 echo "== version $$h ($$n): header, library.json, library.properties, README line and history agree"

clean:
	rm -rf $(OUT)

.PHONY: test version config_check platform_check static_init_check clean
