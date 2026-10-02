# Host-side unit tests (G1 gate, coding plan 5): build with any C99 compiler.
#   make -C test/host        (or: cc -Icomponents/s3_proto ... directly)
CC      ?= gcc
CFLAGS  ?= -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter
INC     := -I../../components/s3_proto -I../../components/s3_ota -I../../components/s3_sf

TESTS   := test_proto test_sha512 test_ed25519 test_bundle test_sf
RUN_TARGETS := $(addprefix run-,$(TESTS))

all: $(TESTS)

test_proto: test_proto.c ../../components/s3_proto/proto_frames.c
	$(CC) $(CFLAGS) $(INC) -o $@ $^

test_sha512: test_sha512.c ../../components/s3_ota/sha512.c
	$(CC) $(CFLAGS) $(INC) -o $@ $^

test_ed25519: test_ed25519.c ../../components/s3_ota/ed25519v.c ../../components/s3_ota/sha512.c
	$(CC) $(CFLAGS) $(INC) -o $@ $^

test_bundle: test_bundle.c ../../components/s3_ota/bundle.c ../../components/s3_ota/sha512.c ../../components/s3_ota/ed25519v.c ../../components/s3_proto/proto_frames.c
	$(CC) $(CFLAGS) $(INC) -o $@ $^ $(LIBS)

test_sf: test_sf.c ../../components/s3_sf/sf_frame.c ../../components/s3_proto/proto_frames.c
	$(CC) $(CFLAGS) $(INC) -I../../components/s3_sf -o $@ $^

run-: ;@true
run-%: %
	./$<

check: $(TESTS) $(RUN_TARGETS)

clean:
	rm -f $(TESTS)

.PHONY: all check clean
