# Makefile for apple-perf-stat
# Builds on macOS (Apple Silicon and Intel)

CC = clang
CFLAGS = -O2 -Wall -Wextra -std=c11

# Check we're on macOS
UNAME_S := $(shell uname -s)

ifneq ($(UNAME_S),Darwin)
$(error This tool only works on macOS)
endif

# Main target uses PET for accurate multi-thread measurement
TARGET = mperf-stat
SRC = apple_perf_stat.c

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

install: $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)

uninstall:
	rm -f $(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)
