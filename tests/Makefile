# Host-side tests for the portable parts of 3dsDL (source/core, source/net and
# the vendored quirc). They build with the system compiler, libcurl, zlib,
# libarchive and zstd, and run under AddressSanitizer and UBSan by default.
#
#   make -C tests check          unit, integration and script tests
#   make -C tests unit           unit tests only
#   make -C tests integration    download engine and ZIP tests (needs python3
#                                and openssl for the fixture server)
#   make -C tests scripts        the build scripts' version rules
#   make -C tests SAN=           build without sanitizers
#
# TMPDIR has to ignore case, as the SD card's FAT does: macOS's does, and
# on Linux an ext4 folder made with casefold does, as CI sets up.

CC       ?= cc
CXX      ?= c++
ROOT     := ..
CORE     := $(ROOT)/source/core
NET      := $(ROOT)/source/net
QUIRC    := $(ROOT)/source/quirc
STB      := $(ROOT)/source/stb
BUILD    := build

SAN      ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined \
            -fno-omit-frame-pointer
WARN     := -Wall -Wextra
CPPFLAGS := -I$(CORE) -I$(NET) -I$(QUIRC) -I$(STB)
LDLIBS   := -lcurl -larchive -lzstd -lz -lm

# Homebrew keeps libarchive and zstd out of the compiler's paths on macOS.
ARCHIVE_PREFIX := $(shell brew --prefix libarchive 2>/dev/null)
ifneq ($(ARCHIVE_PREFIX),)
CPPFLAGS += -I$(ARCHIVE_PREFIX)/include
LDLIBS   := -L$(ARCHIVE_PREFIX)/lib $(LDLIBS)
endif
ZSTD_PREFIX := $(shell brew --prefix zstd 2>/dev/null)
ifneq ($(ZSTD_PREFIX),)
CPPFLAGS += -I$(ZSTD_PREFIX)/include
LDLIBS   := -L$(ZSTD_PREFIX)/lib $(LDLIBS)
endif

CXXFLAGS := -std=c++17 -g -O1 $(WARN) -Werror $(SAN) $(CPPFLAGS)
CFLAGS   := -std=gnu99 -g -O1 $(WARN) $(SAN) $(CPPFLAGS)

CORE_SRC        := $(wildcard $(CORE)/*.cpp)
NET_SRC         := $(wildcard $(NET)/*.cpp)
UNIT_SRC        := $(wildcard unit/*.cpp)
INTEGRATION_SRC := $(wildcard integration/*.cpp)
QUIRC_OBJ       := $(patsubst $(QUIRC)/%.c,$(BUILD)/quirc/%.o,$(wildcard $(QUIRC)/*.c))
HEADERS         := $(wildcard $(CORE)/*.hpp $(NET)/*.hpp $(QUIRC)/*.h *.hpp)

.PHONY: all check unit integration scripts clean

all: $(BUILD)/unit_tests $(BUILD)/integration_tests

check: unit integration scripts

unit: $(BUILD)/unit_tests
	./$(BUILD)/unit_tests

integration: $(BUILD)/integration_tests
	./run_integration.sh ./$(BUILD)/integration_tests

scripts:
	./version_test.sh

# quirc is vendored, so its upstream warnings are not ours to fix.
# quirc_resize() calls memcpy(dst, NULL, 0) on first use, which UBSan's
# nonnull check reports; that one check is relaxed for quirc only.
$(BUILD)/quirc/%.o: $(QUIRC)/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-sign-compare -Wno-unused-parameter -Wno-type-limits \
		-fno-sanitize=nonnull-attribute -c $< -o $@

$(BUILD)/qrcodegen.o: $(ROOT)/source/qrcodegen/qrcodegen.c $(ROOT)/source/qrcodegen/qrcodegen.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@

$(BUILD)/unit_tests: $(UNIT_SRC) $(CORE_SRC) $(NET_SRC) $(QUIRC_OBJ) $(BUILD)/qrcodegen.o $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(UNIT_SRC) $(CORE_SRC) $(NET_SRC) $(QUIRC_OBJ) $(BUILD)/qrcodegen.o -o $@ $(LDLIBS)

$(BUILD)/integration_tests: $(INTEGRATION_SRC) $(CORE_SRC) $(NET_SRC) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INTEGRATION_SRC) $(CORE_SRC) $(NET_SRC) -o $@ $(LDLIBS)

clean:
	rm -rf $(BUILD)
