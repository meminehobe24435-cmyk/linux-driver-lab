# SPDX-License-Identifier: MIT
#
# linux-driver-lab 顶层 Makefile
#
# 这是一个 kbuild 外部模块 Makefile：同一份文件被 make 调用两次。
#   - 第一次（KERNELRELEASE 为空）：从命令行进来，转手交给内核构建系统
#   - 第二次（KERNELRELEASE 非空）：由 kbuild 在内部调用，此时才真正定义模块
#
# 常用目标：
#   make                构建 labdev.ko
#   make test-host      在用户态编译并运行环形缓冲区单元测试（不需要内核头文件）
#   make tools          编译 tools/dtest
#   make check          跑 test-host + 编译 dtest
#   make load/unload    加载/卸载模块（需要 root 且内核版本匹配）
#   make clean          清理构建产物

ifneq ($(KERNELRELEASE),)

# ---- 由 kbuild 内部调用 ----
obj-m := labdev.o
labdev-y := labdev_main.o ringbuf.o

# 这里刻意**不**加 -Wextra：内核头文件里大量 static inline 桩函数并不满足
# -Wextra（会刷出一屏 unused-parameter），而 kbuild 默认已经带了 -Wall 以及
# 一堆更严格的检查（-Wundef、-Werror=implicit-function-declaration 等）。
# 目标就是：默认 CFLAGS 下编译零 warning。

else

# ---- 从命令行调用 ----

# KDIR：内核构建目录。默认用当前运行内核；本机内核与头文件不匹配时
# 可以显式指定，例如：
#   make KDIR=/lib/modules/$(uname -r)/build
#   make KDIR=/usr/src/linux-headers-5.15.0-194-generic
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

DEVNAME := labdev

.PHONY: all modules clean tools test-host check load unload test help

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
	$(MAKE) -C tools clean
	$(MAKE) -C tests clean

tools:
	$(MAKE) -C tools

test-host:
	$(MAKE) -C tests test

check: test-host tools

# 下面三个目标会真正动内核，必须有 root 权限
load: modules
	sudo insmod labdev.ko
	@test -c /dev/$(DEVNAME) || { echo "错误: /dev/$(DEVNAME) 未出现"; exit 1; }
	@echo "已加载: /dev/$(DEVNAME)"

unload:
	-sudo rmmod labdev
	@test ! -c /dev/$(DEVNAME) || { echo "错误: /dev/$(DEVNAME) 仍然存在"; exit 1; }
	@echo "已卸载, 设备节点已消失"

help:
	@echo "make            构建 labdev.ko"
	@echo "make test-host  用户态环形缓冲区单元测试"
	@echo "make tools      编译 tools/dtest"
	@echo "make check      test-host + tools"
	@echo "make load       加载模块并检查 /dev/$(DEVNAME)"
	@echo "make unload     卸载模块并检查节点消失"
	@echo "make clean      清理"

endif
