# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
#
#   make all            library objects + unit test + integration test
#   make check          unit tests
#   make fuzz           unit tests again under AddressSanitizer + UBSan (the unit tests include XML/HTTP mutation runs)
#   make integ-asan     integration test (needs a server: see test/run.sh) built with ASan + UBSan
#   make MBEDTLS=/path/to/mbedtls-3.6 tls   also builds the TLS adapter
CC ?= gcc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
SRC = src/phedav_xml.c src/phedav_http.c src/phedav_dav.c src/phedav_sync.c
HDR = include/phedav.h include/phedav_xml.h
INC = -Iinclude -Itest
POSIX = -D_POSIX_C_SOURCE=200809L

all: build/unit build/integ

build:
	mkdir -p build

build/unit: $(SRC) $(HDR) test/unit.c | build
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC) test/unit.c

build/integ: $(SRC) $(HDR) test/integ.c test/posix_net.c test/devdb.c test/devdb.h test/posix_net.h | build
	$(CC) $(CFLAGS) $(POSIX) $(INC) -o $@ $(SRC) test/integ.c test/posix_net.c test/devdb.c

check: build/unit
	./build/unit

fuzz: $(SRC) $(HDR) test/unit.c | build
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(INC) -o build/unit_asan $(SRC) test/unit.c
	./build/unit_asan

integ-asan: $(SRC) $(HDR) test/integ.c test/posix_net.c test/devdb.c | build
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(POSIX) $(INC) -o build/integ_asan $(SRC) test/integ.c test/posix_net.c test/devdb.c

ifneq ($(MBEDTLS),)
MBEDLIBS = $(MBEDTLS)/library/libmbedtls.a $(MBEDTLS)/library/libmbedx509.a $(MBEDTLS)/library/libmbedcrypto.a
# the TLS adapter on its own (compile check) and the integration test with it (PHEDAV_CA=<pem> or PHEDAV_PIN=<sha256 hex>)
tls: build
	$(CC) $(CFLAGS) -isystem $(MBEDTLS)/include $(INC) -c -o build/phedav_mbedtls.o src/phedav_mbedtls.c

integ-tls: $(SRC) $(HDR) src/phedav_mbedtls.c include/phedav_mbedtls.h test/integ.c test/posix_net.c test/devdb.c | build
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(POSIX) -DPHEDAV_WITH_MBEDTLS 	    -isystem $(MBEDTLS)/include $(INC) -o build/integ_tls $(SRC) src/phedav_mbedtls.c test/integ.c test/posix_net.c test/devdb.c $(MBEDLIBS)
endif

clean:
	rm -rf build
