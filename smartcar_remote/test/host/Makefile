# 主机侧自测（G1 门，无需目标机）
#
#   make        # 编译并运行
#   make check  # 同上，带通过断言
#
# 对齐 esp32c6_car test/host 的角色：协议编解码是纯 C99，同一份源文件
# （main/proto/proto_frames.[ch]，与 esp32c6_car 逐字节同源）直接在主机编译。

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -std=c99
PROTO_SRCS := ../../main/proto/proto_frames.c
PROTO_INCS := -I../../main/proto

all: run

test_proto: test_proto.c $(PROTO_SRCS)
	$(CC) $(CFLAGS) $(PROTO_INCS) test_proto.c $(PROTO_SRCS) -o $@

# header-only codec（cam_frame.h / vision.h），只需头文件目录
test_cam_frame: test_cam_frame.c ../../main/proto/cam_frame.h
	$(CC) $(CFLAGS) $(PROTO_INCS) test_cam_frame.c -o $@

test_vision: test_vision.c ../../main/proto/vision.h
	$(CC) $(CFLAGS) $(PROTO_INCS) test_vision.c -o $@

run: test_proto test_cam_frame test_vision
	./test_proto
	./test_cam_frame
	./test_vision

check: run

clean:
	rm -f test_proto test_cam_frame test_vision

.PHONY: all run check clean
