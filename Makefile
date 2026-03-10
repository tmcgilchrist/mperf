# Makefile for apple-perf-stat
# Only builds on macOS with Apple Silicon

CC = clang
CFLAGS = -O2 -Wall -Wextra -std=c11

# Check we're on macOS ARM64
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifneq ($(UNAME_S),Darwin)
$(error This tool only works on macOS)
endif

ifneq ($(UNAME_M),arm64)
$(warning This tool is designed for Apple Silicon, may not work on Intel)
endif

# Main target uses PET for accurate multi-thread measurement
TARGET = mperf-stat
SRC = apple_perf_stat.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)