# host unit tests for the tc275_sbl OTA software (doc 24 gates G-OTA-1/2,
# plus meta/boot/rx logic). Any C99 compiler works; on this bench:
#   /c/msys64/ucrt64/bin/gcc
#
# The TCFW test vectors are generated from the esp32c6_car dev key:
#   python tools/gen_test_vectors.py
# (test_vectors.h is committed, regeneration is only needed when the bundle
# format or the key changes.)

CC      = C:/msys64/ucrt64/bin/gcc
# (plain assignment: "?=" would not override make's builtin CC = cc)
CFLAGS  ?= -std=c99 -Wall -Wextra -O2
CFLAGS  += -I. -I../.. -I../../mw/ota -I../../mw/crypto -I../../mw/sf

PORTABLE = \
    ../../mw/ota/crc32.c \
    ../../mw/ota/ota_meta.c \
    ../../mw/ota/ota_boot.c \
    ../../mw/ota/tcfw_bundle.c \
    ../../mw/ota/ota_rx.c \
    ../../mw/crypto/sha512.c \
    ../../mw/crypto/ed25519v.c \
    sf_frame.c \
    mock_ota.c

TESTS = test_tcfw test_ota_frames test_ota_meta test_ota_boot test_ota_rx

all: $(TESTS)

test_tcfw: test_tcfw.c $(PORTABLE)
	$(CC) $(CFLAGS) -o $@ test_tcfw.c $(PORTABLE)

test_ota_frames: test_ota_frames.c $(PORTABLE)
	$(CC) $(CFLAGS) -o $@ test_ota_frames.c $(PORTABLE)

test_ota_meta: test_ota_meta.c $(PORTABLE)
	$(CC) $(CFLAGS) -o $@ test_ota_meta.c $(PORTABLE)

test_ota_boot: test_ota_boot.c $(PORTABLE)
	$(CC) $(CFLAGS) -o $@ test_ota_boot.c $(PORTABLE)

test_ota_rx: test_ota_rx.c $(PORTABLE)
	$(CC) $(CFLAGS) -o $@ test_ota_rx.c $(PORTABLE)

check: $(TESTS)
	@fail=0; \
	for t in $(TESTS); do echo "== $$t"; ./$$t || fail=1; done; \
	exit $$fail

clean:
	rm -f $(TESTS)

.PHONY: all check clean
