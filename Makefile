CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=gnu11 -Wall -Wextra
GTK_CF  := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LD  := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)

# platform backends: macOS (Mach) or Linux (/proc + libutil for forkpty)
ifeq ($(shell uname -s),Darwin)
SYSINFO := src/sysinfo_mac.c
PTYLIB  := -lm
else
SYSINFO := src/sysinfo.c
PTYLIB  := -lutil -lm
endif

all: hterm

hterm: src/main.c src/history.c src/vt.c $(SYSINFO) src/*.h
	@pkg-config --exists gtk+-3.0 || { echo "GTK3 not found. macOS: brew install gtk+3 pkg-config"; exit 1; }
	$(CC) $(CFLAGS) $(GTK_CF) src/main.c src/history.c src/vt.c $(SYSINFO) -o $@ $(GTK_LD) $(PTYLIB)

test: tests/test_history tests/test_sysinfo tests/test_vt
	./tests/test_history && ./tests/test_sysinfo && ./tests/test_vt

tests/test_history: tests/test_history.c src/history.c
	$(CC) $(CFLAGS) $^ -o $@
tests/test_sysinfo: tests/test_sysinfo.c $(SYSINFO)
	$(CC) $(CFLAGS) $^ -o $@
tests/test_vt: tests/test_vt.c src/vt.c
	$(CC) $(CFLAGS) $^ -o $@ $(PTYLIB)

clean:
	rm -f hterm tests/test_history tests/test_sysinfo tests/test_vt
.PHONY: all test clean
