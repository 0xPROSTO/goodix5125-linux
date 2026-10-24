CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fPIC -Ilib \
           $(shell pkg-config --cflags libusb-1.0 openssl)
LDLIBS  += $(shell pkg-config --libs libusb-1.0 openssl) -lm

LIB_SRC := lib/proto.c lib/device.c lib/usb.c lib/tls.c lib/sensor.c lib/image.c lib/match.c
LIB_OBJ := $(LIB_SRC:lib/%.c=build/%.o)

all: build/gxctl build/test build/matcheval build/seteval

build/%.o: lib/%.c lib/gx5125.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/libgx5125.a: $(LIB_OBJ)
	ar rcs $@ $^

build/gxctl: tools/gxctl.c build/libgx5125.a
	$(CC) $(CFLAGS) $< build/libgx5125.a $(LDLIBS) -o $@

build/test: tests/test.c build/libgx5125.a
	$(CC) $(CFLAGS) $< build/libgx5125.a $(LDLIBS) -o $@

check: build/test
	./build/test

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all check clean

build/matcheval: tools/matcheval.c build/libgx5125.a
	$(CC) $(CFLAGS) $< build/libgx5125.a $(LDLIBS) -o $@

build/seteval: tools/seteval.c build/libgx5125.a
	$(CC) $(CFLAGS) $< build/libgx5125.a $(LDLIBS) -o $@
