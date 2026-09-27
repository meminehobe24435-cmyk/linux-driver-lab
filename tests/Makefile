# SPDX-License-Identifier: MIT
#
# 环形缓冲区核心逻辑的用户态单元测试
#
# 这里编译的是与内核模块完全相同的那份 ringbuf.c —— 只是把
# 内存分配从 kvmalloc() 换成了 malloc()。所以这些测试覆盖的是**真实参与
# 内核模块的算法**，但它们本身仍然是用户态测试，不是内核态测试。

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
CPPFLAGS += -I..

BIN := ringbuf_test
SRC := ringbuf_test.c ../ringbuf.c

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRC) ../ringbuf.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRC)

test: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN) *.o
