# SPDX-License-Identifier: MIT
#
# tools/dtest 使用手册：直接调用 ioctl，所以只需要 C 库，不依赖内核头文件。
# （lab_ioctl.h 只用到了 <linux/types.h> 里的 __u32/__u64，由 linux-libc-dev 提供。）

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
CPPFLAGS += -I..

BIN := dtest

.PHONY: all clean

all: $(BIN)

$(BIN): dtest.c ../lab_ioctl.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ dtest.c

clean:
	rm -f $(BIN) *.o
