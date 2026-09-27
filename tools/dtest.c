// SPDX-License-Identifier: MIT
/*
 * dtest.c - labdev 字符设备的用户态功能测试
 *
 * 覆盖：open/release、read/write、非阻塞与阻塞读、全部 4 个 ioctl、
 *       poll/epoll、llseek 回退、以及各种边界与错误路径。
 *
 * 用法:  sudo ./tools/dtest [/dev/labdev]
 * 退出码: 0 = 全部 PASS, 1 = 有 FAIL
 *
 * 注意：这个程序必须在内核模块已经 insmod 之后运行。
 *       它测的是**真实的内核态行为**，不是用户态模拟。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/types.h>

#include "lab_ioctl.h"

static const char *g_dev = "/dev/labdev";

static int g_checks;
static int g_cases;
static int g_cases_failed;
static int g_case_failed;

#define CHECK(cond, ...)						\
	do {								\
		g_checks++;						\
		if (!(cond)) {						\
			g_case_failed = 1;				\
			printf("      ! ");				\
			printf(__VA_ARGS__);				\
			printf("\n");					\
		}							\
	} while (0)

static void case_begin(void)
{
	g_case_failed = 0;
}

static void case_end(const char *name)
{
	g_cases++;
	if (g_case_failed) {
		g_cases_failed++;
		printf("[FAIL] %s\n", name);
	} else {
		printf("[PASS] %s\n", name);
	}
}

/* ------------------------------------------------------------------ */
/* ioctl 薄封装                                                        */
/* ------------------------------------------------------------------ */

static int ioc_get_config(int fd, struct lab_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	return ioctl(fd, LAB_IOC_GET_CONFIG, cfg);
}

/* 返回 ioctl 的返回值；成功时把内核回填的配置写进 applied */
static int ioc_set_config(int fd, const struct lab_config *want,
			  struct lab_config *applied)
{
	struct lab_config cfg = *want;

	if (ioctl(fd, LAB_IOC_SET_CONFIG, &cfg) < 0)
		return -1;
	if (applied)
		*applied = cfg;
	return 0;
}

static int ioc_reset(int fd)
{
	return ioctl(fd, LAB_IOC_RESET, 0);
}

static int ioc_get_stats(int fd, struct lab_stats *st)
{
	memset(st, 0, sizeof(*st));
	return ioctl(fd, LAB_IOC_GET_STATS, st);
}

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static unsigned char g_a[1 << 20];
static unsigned char g_b[1 << 20];
static unsigned char g_c[1 << 20];

static void fill_pattern(unsigned char *p, size_t n, unsigned int seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		p[i] = (unsigned char)((i * 31 + seed * 17 + (i >> 8)) & 0xFF);
}

static const char *errname(void)
{
	static char buf[64];

	switch (errno) {
	case EAGAIN:	return "EAGAIN";
	case EINVAL:	return "EINVAL";
	case ENOTTY:	return "ENOTTY";
	case EFAULT:	return "EFAULT";
	case ENOSPC:	return "ENOSPC";
	default:
		snprintf(buf, sizeof(buf), "errno=%d", errno);
		return buf;
	}
}

/* 把缓冲区恢复到已知的干净状态，避免用例之间互相影响 */
static int reset_to(int fd, unsigned int buf_size, unsigned int flags,
		    unsigned int read_chunk)
{
	struct lab_config cfg, applied;

	if (ioc_reset(fd) < 0)
		return -1;
	memset(&cfg, 0, sizeof(cfg));
	cfg.buffer_size = buf_size;
	cfg.read_chunk = read_chunk;
	cfg.flags = flags;
	if (ioc_set_config(fd, &cfg, &applied) < 0)
		return -1;
	if (applied.buffer_size != buf_size || applied.flags != flags ||
	    applied.read_chunk != read_chunk)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* 用例                                                                */
/* ------------------------------------------------------------------ */

static void t_open_and_defaults(void)
{
	struct lab_config cfg;
	struct lab_stats st;
	int fd;

	case_begin();
	fd = open(g_dev, O_RDWR);
	CHECK(fd >= 0, "open(%s, O_RDWR) 失败: %s", g_dev, strerror(errno));
	if (fd < 0) {
		case_end("打开设备并读取默认配置");
		return;
	}

	CHECK(ioc_get_config(fd, &cfg) == 0, "GET_CONFIG 失败: %s", errname());
	CHECK(cfg.buffer_size >= LAB_MIN_BUF && cfg.buffer_size <= LAB_MAX_BUF,
	      "buffer_size=%u 不在合法范围 [%u,%u]",
	      cfg.buffer_size, LAB_MIN_BUF, LAB_MAX_BUF);
	CHECK(cfg.reserved == 0, "reserved 必须为 0, 实际 %u", cfg.reserved);
	printf("      · 当前 buffer_size=%u read_chunk=%u flags=0x%x\n",
	       cfg.buffer_size, cfg.read_chunk, cfg.flags);

	CHECK(ioc_reset(fd) == 0, "RESET 失败: %s", errname());
	CHECK(ioc_get_stats(fd, &st) == 0, "GET_STATS 失败: %s", errname());
	CHECK(st.bytes_written == 0 && st.bytes_read == 0 &&
	      st.bytes_dropped == 0 && st.wait_count == 0 &&
	      st.poll_count == 0 && st.write_calls == 0 && st.read_calls == 0,
	      "RESET 之后 7 个数据计数器应全为 0, 实际 written=%llu read=%llu "
	      "dropped=%llu wait=%llu poll=%llu wc=%llu rc=%llu",
	      st.bytes_written, st.bytes_read, st.bytes_dropped, st.wait_count,
	      st.poll_count, st.write_calls, st.read_calls);

	close(fd);
	case_end("打开设备 + GET_CONFIG/RESET/GET_STATS 默认值");
}

static void t_roundtrip(void)
{
	struct lab_stats st;
	ssize_t n;
	int fd;

	case_begin();
	fd = open(g_dev, O_RDWR);
	if (fd < 0 || reset_to(fd, 4096, 0, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd >= 0)
			close(fd);
		case_end("write/read 往返数据一致");
		return;
	}

	fill_pattern(g_a, 100, 1);
	memset(g_b, 0, 100);

	n = write(fd, g_a, 100);
	CHECK(n == 100, "write 100 字节应返回 100, 实际 %zd (%s)", n, errname());
	n = read(fd, g_b, 100);
	CHECK(n == 100, "read 100 字节应返回 100, 实际 %zd (%s)", n, errname());
	CHECK(memcmp(g_a, g_b, 100) == 0, "读出内容与写入不一致");

	CHECK(ioc_get_stats(fd, &st) == 0, "GET_STATS 失败");
	CHECK(st.bytes_written == 100, "bytes_written 应为 100, 实际 %llu", st.bytes_written);
	CHECK(st.bytes_read == 100, "bytes_read 应为 100, 实际 %llu", st.bytes_read);
	CHECK(st.bytes_dropped == 0, "不应有丢弃, 实际 %llu", st.bytes_dropped);
	CHECK(st.write_calls >= 1 && st.read_calls >= 1, "调用计数未增长");

	close(fd);
	case_end("write/read 往返 + 统计计数");
}

static void t_nonblock_empty_read(void)
{
	ssize_t n;
	int fd;

	case_begin();
	fd = open(g_dev, O_RDWR | O_NONBLOCK);
	if (fd < 0 || reset_to(fd, 4096, 0, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd >= 0)
			close(fd);
		case_end("O_NONBLOCK 空缓冲区读返回 -EAGAIN");
		return;
	}

	errno = 0;
	n = read(fd, g_b, 16);
	CHECK(n == -1, "空缓冲区非阻塞读应失败, 实际返回 %zd", n);
	CHECK(errno == EAGAIN, "errno 应为 EAGAIN, 实际 %s", errname());

	close(fd);
	case_end("O_NONBLOCK 空缓冲区读返回 -EAGAIN");
}

static void t_blocking_read_wakeup(void)
{
	struct lab_stats st, st_before;
	ssize_t n;
	pid_t pid;
	int status;
	int fd;

	case_begin();
	fd = open(g_dev, O_RDWR);
	if (fd < 0 || reset_to(fd, 4096, 0, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd >= 0)
			close(fd);
		case_end("阻塞读被写入唤醒");
		return;
	}
	(void)ioc_get_stats(fd, &st_before);

	fill_pattern(g_a, 16, 7);
	memset(g_b, 0, 16);

	fflush(NULL);
	pid = fork();
	CHECK(pid >= 0, "fork 失败: %s", strerror(errno));
	if (pid == 0) {
		/* 子进程：先阻塞在 read 上，等父进程写入后才返回 */
		ssize_t got;

		alarm(10);	/* 兜底：万一永远醒不过来，靠信号结束而不是挂死 CI */
		got = read(fd, g_b, 16);
		if (got != 16)
			_exit(1);
		if (memcmp(g_a, g_b, 16) != 0)
			_exit(2);
		_exit(0);
	}

	/* 父进程：确认子进程已经睡下去，再写入把它叫醒 */
	usleep(300 * 1000);
	n = write(fd, g_a, 16);
	CHECK(n == 16, "父进程 write 应返回 16, 实际 %zd (%s)", n, errname());

	CHECK(waitpid(pid, &status, 0) == pid, "waitpid 失败");
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "阻塞读的子进程异常: exited=%d status=%d signal=%d",
	      WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
	      WIFSIGNALED(status) ? WTERMSIG(status) : 0);

	CHECK(ioc_get_stats(fd, &st) == 0, "GET_STATS 失败");
	CHECK(st.wait_count >= st_before.wait_count + 1,
	      "wait_count 应至少 +1 (前 %llu 后 %llu)",
	      st_before.wait_count, st.wait_count);

	close(fd);
	case_end("阻塞读: 无数据时睡眠, 写入后唤醒并读到正确数据");
}

static void t_poll_semantics(void)
{
	struct pollfd pfd;
	ssize_t n;
	int ret;
	int fd;

	case_begin();
	fd = open(g_dev, O_RDWR);
	if (fd < 0 || reset_to(fd, 4096, 0, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd >= 0)
			close(fd);
		case_end("poll: 空缓冲区无 EPOLLIN, 有数据时上报 EPOLLIN");
		return;
	}

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = fd;
	pfd.events = POLLIN | POLLOUT;

	ret = poll(&pfd, 1, 0);
	CHECK(ret == 1, "poll 应返回 1, 实际 %d", ret);
	CHECK(!(pfd.revents & POLLIN), "空缓冲区不应上报 POLLIN, revents=0x%x", pfd.revents);
	CHECK(pfd.revents & POLLOUT, "设备永远可写, 应上报 POLLOUT, revents=0x%x", pfd.revents);

	CHECK(write(fd, "hello", 5) == 5, "write 失败: %s", errname());
	pfd.revents = 0;
	ret = poll(&pfd, 1, 0);
	CHECK(ret == 1 && (pfd.revents & POLLIN),
	      "有数据时应上报 POLLIN, revents=0x%x", pfd.revents);

	n = read(fd, g_b, 5);
	CHECK(n == 5, "read 失败: %s", errname());
	pfd.revents = 0;
	ret = poll(&pfd, 1, 0);
	CHECK(ret == 1 && !(pfd.revents & POLLIN),
	      "读空后不应再上报 POLLIN, revents=0x%x", pfd.revents);

	close(fd);
	case_end("poll: 空时无 EPOLLIN, 有数据时立即上报 EPOLLIN");
}

static void t_epoll_level_triggered(void)
{
	struct epoll_event ev, evs[4];
	struct lab_stats st;
	int epfd, ret, fd;

	case_begin();
	fd = open(g_dev, O_RDWR);
	if (fd < 0 || reset_to(fd, 4096, 0, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd >= 0)
			close(fd);
		case_end("epoll: 水平触发下 EPOLLIN 状态跟随缓冲区");
		return;
	}

	epfd = epoll_create1(0);
	CHECK(epfd >= 0, "epoll_create1 失败: %s", strerror(errno));

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.fd = fd;
	CHECK(epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == 0,
	      "epoll_ctl(ADD) 失败: %s", strerror(errno));

	ret = epoll_wait(epfd, evs, 4, 0);
	CHECK(ret == 0, "空缓冲区 epoll_wait 应返回 0, 实际 %d", ret);

	CHECK(write(fd, "abc", 3) == 3, "write 失败: %s", errname());
	ret = epoll_wait(epfd, evs, 4, 0);
	CHECK(ret == 1 && (evs[0].events & EPOLLIN),
	      "有数据时 epoll_wait 应返回 EPOLLIN, ret=%d events=0x%x",
	      ret, ret == 1 ? evs[0].events : 0);

	CHECK(read(fd, g_b, 3) == 3, "read 失败: %s", errname());
	ret = epoll_wait(epfd, evs, 4, 0);
	CHECK(ret == 0, "读空后 epoll_wait 应返回 0, 实际 %d", ret);

	/* 阻塞等待：注册 EPOLLOUT 后设备一直可写，应当立即返回 */
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLOUT;
	ev.data.fd = fd;
	CHECK(epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == 0,
	      "epoll_ctl(MOD) 失败: %s", strerror(errno));
	ret = epoll_wait(epfd, evs, 4, 200);
	CHECK(ret == 1 && (evs[0].events & EPOLLOUT),
	      "注册 EPOLLOUT 后应立刻可写, ret=%d", ret);

	CHECK(ioc_get_stats(fd, &st) == 0, "GET_STATS 失败");
	/*
	 * epoll 只在"就绪链表非空"或"重新轮询状态"时才会走到驱动的 .poll，
	 * 具体次数依赖内核实现（ep_insert/ep_send_events/ep_modify 各会调一次），
	 * 所以这里只断言"poll 确实被 epoll 调用过多次"，不锁死精确数字。
	 */
	CHECK(st.poll_count >= 3,
	      "poll_count 说明 epoll 确实调用了驱动的 poll, 实际 %llu", st.poll_count);

	close(epfd);
	close(fd);
	case_end("epoll: 水平触发 + EPOLLOUT 始终就绪");
}

static void t_llseek(void)
{
	ssize_t n;
	off_t off;
	int fd;

	case_begin();
	fd = open(g_dev, O_RDWR);
	if (fd < 0 || reset_to(fd, 4096, 0, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd >= 0)
			close(fd);
		case_end("llseek: 读游标可查询可回退, 越界被拒");
		return;
	}

	fill_pattern(g_a, 100, 3);
	CHECK(write(fd, g_a, 100) == 100, "write 100 失败: %s", errname());

	memset(g_b, 0, 100);
	CHECK(read(fd, g_b, 40) == 40, "先读 40 字节失败: %s", errname());
	CHECK(memcmp(g_b, g_a, 40) == 0, "前 40 字节内容不符");

	off = lseek(fd, 0, SEEK_SET);
	CHECK(off == 0, "lseek(SEEK_SET,0) 应返回 0, 实际 %ld (%s)", (long)off, errname());
	off = lseek(fd, 0, SEEK_CUR);
	CHECK(off == 0, "lseek(SEEK_CUR,0) 应返回 0, 实际 %ld", (long)off);
	off = lseek(fd, 0, SEEK_END);
	CHECK(off == 100, "lseek(SEEK_END,0) 应返回 100, 实际 %ld", (long)off);

	/* 关键：回退之后必须能重新读出原始数据 */
	CHECK(lseek(fd, 0, SEEK_SET) == 0, "回退到开头失败");
	memset(g_c, 0, 100);
	n = read(fd, g_c, 100);
	CHECK(n == 100, "回退后应能重读 100 字节, 实际 %zd (%s)", n, errname());
	CHECK(memcmp(g_c, g_a, 100) == 0, "回退重读的内容与原始写入不一致");

	CHECK(lseek(fd, 50, SEEK_SET) == 50, "lseek(SEEK_SET,50) 失败");
	memset(g_c, 0, 20);
	CHECK(read(fd, g_c, 10) == 10, "从 50 处读 10 字节失败");
	CHECK(memcmp(g_c, g_a + 50, 10) == 0, "从 50 处读到的内容应为 a[50..59]");
	off = lseek(fd, 0, SEEK_CUR);
	CHECK(off == 60, "读 10 字节后 SEEK_CUR 应为 60, 实际 %ld", (long)off);

	errno = 0;
	off = lseek(fd, 101, SEEK_SET);
	CHECK(off == -1 && errno == EINVAL,
	      "超出末尾的 SEEK_SET 应返回 -EINVAL, 实际 off=%ld %s", (long)off, errname());
	errno = 0;
	off = lseek(fd, -1, SEEK_SET);
	CHECK(off == -1 && errno == EINVAL,
	      "负位置应返回 -EINVAL, 实际 off=%ld %s", (long)off, errname());
	errno = 0;
	off = lseek(fd, 0, 99);
	CHECK(off == -1 && errno == EINVAL,
	      "非法 whence 应返回 -EINVAL, 实际 off=%ld %s", (long)off, errname());

	close(fd);
	case_end("llseek: 回退重读 + 越界/非法 whence 返回 -EINVAL");
}

static void t_ioctl_config_and_chunk(void)
{
	struct lab_config cfg, applied;
	struct lab_stats st;
	ssize_t n;
	int fd;

	case_begin();
	fd = open(g_dev, O_RDWR);
	if (fd < 0 || reset_to(fd, 4096, 0, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd >= 0)
			close(fd);
		case_end("SET_CONFIG 的 read_chunk 生效 + IOWR 回填");
		return;
	}

	/* read_chunk 限制单次 read 的最大返回长度 */
	fill_pattern(g_a, 500, 5);
	CHECK(write(fd, g_a, 500) == 500, "write 500 失败: %s", errname());

	memset(&cfg, 0, sizeof(cfg));
	cfg.buffer_size = 0;	/* 0 = 保持不变 */
	cfg.read_chunk = 100;
	CHECK(ioc_set_config(fd, &cfg, &applied) == 0, "SET_CONFIG 失败: %s", errname());
	CHECK(applied.buffer_size == 4096,
	      "buffer_size 传 0 应保持 4096, 实际回填 %u", applied.buffer_size);
	CHECK(applied.read_chunk == 100, "回填的 read_chunk 应为 100, 实际 %u",
	      applied.read_chunk);

	n = read(fd, g_b, 500);
	CHECK(n == 100, "read_chunk=100 时 read 应返回 100, 实际 %zd", n);
	CHECK(memcmp(g_b, g_a, 100) == 0, "被截断读出的数据内容不符");

	CHECK(ioc_get_config(fd, &cfg) == 0, "GET_CONFIG 失败");
	CHECK(cfg.read_chunk == 100, "GET_CONFIG 应读到 read_chunk=100, 实际 %u",
	      cfg.read_chunk);
	CHECK((int)cfg.buffer_size == (int)applied.buffer_size,
	      "GET_CONFIG 与 SET_CONFIG 的 buffer_size 不一致");

	/* 取消限制后应能一次读完剩下的 400 字节 */
	memset(&cfg, 0, sizeof(cfg));
	cfg.read_chunk = 0;
	CHECK(ioc_set_config(fd, &cfg, &applied) == 0, "SET_CONFIG 失败: %s", errname());
	n = read(fd, g_b, 500);
	CHECK(n == 400, "取消 read_chunk 后应一次读回 400, 实际 %zd", n);
	CHECK(memcmp(g_b, g_a + 100, 400) == 0, "剩余数据内容不符");

	CHECK(ioc_get_stats(fd, &st) == 0, "GET_STATS 失败");
	CHECK(st.bytes_read == 500, "bytes_read 应为 500, 实际 %llu", st.bytes_read);

	close(fd);
	case_end("SET_CONFIG(read_chunk) 生效 + _IOWR 回填实际配置");
}

static void t_ioctl_resize(void)
{
	struct lab_config cfg, applied;
	struct lab_stats st, before;
	ssize_t n;
	int fd;

	case_begin();
	fd = open(g_dev, O_RDWR);
	if (fd < 0 || reset_to(fd, 4096, 0, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd >= 0)
			close(fd);
		case_end("SET_CONFIG(buffer_size) 扩容保数据 / 缩容计丢弃");
		return;
	}

	/* 扩容：数据必须完好保留 */
	fill_pattern(g_a, 3000, 9);
	CHECK(write(fd, g_a, 3000) == 3000, "write 3000 失败: %s", errname());
	memset(&cfg, 0, sizeof(cfg));
	cfg.buffer_size = 8192;
	CHECK(ioc_set_config(fd, &cfg, &applied) == 0, "扩容失败: %s", errname());
	CHECK(applied.buffer_size == 8192, "扩容后应回填 8192, 实际 %u",
	      applied.buffer_size);
	CHECK(ioc_get_config(fd, &cfg) == 0 && cfg.buffer_size == 8192,
	      "GET_CONFIG 应看到 8192");
	memset(g_b, 0, 3000);
	n = read(fd, g_b, 3000);
	CHECK(n == 3000, "扩容后应能读回 3000 字节, 实际 %zd", n);
	CHECK(memcmp(g_a, g_b, 3000) == 0, "扩容过程中数据被破坏");

	/* 缩容：装不下的部分计入 bytes_dropped */
	CHECK(ioc_get_stats(fd, &before) == 0, "GET_STATS 失败");
	CHECK(write(fd, g_a, 3000) == 3000, "write 3000 失败: %s", errname());
	memset(&cfg, 0, sizeof(cfg));
	cfg.buffer_size = 1024;
	CHECK(ioc_set_config(fd, &cfg, &applied) == 0, "缩容失败: %s", errname());
	CHECK(applied.buffer_size == 1024, "缩容后应回填 1024, 实际 %u",
	      applied.buffer_size);

	CHECK(ioc_get_stats(fd, &st) == 0, "GET_STATS 失败");
	CHECK(st.bytes_dropped == before.bytes_dropped + (3000 - 1024),
	      "缩容丢弃应为 1976, 实际 %llu",
	      st.bytes_dropped - before.bytes_dropped);

	memset(g_b, 0, 1024);
	n = read(fd, g_b, 1024);
	CHECK(n == 1024, "缩容后应读到 1024 字节, 实际 %zd", n);
	CHECK(memcmp(g_b, g_a, 1024) == 0, "缩容保留的应是数据的前 1024 字节");

	/* 恢复默认，避免影响后续用例 */
	CHECK(reset_to(fd, 4096, 0, 0) == 0, "恢复默认配置失败");

	close(fd);
	case_end("SET_CONFIG(buffer_size): 扩容保留数据, 缩容如实计丢弃");
}

static void t_buffer_full_and_drop(void)
{
	struct lab_stats st;
	ssize_t n;
	int fd;

	case_begin();
	fd = open(g_dev, O_RDWR);
	if (fd < 0 || reset_to(fd, 4096, 0, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd >= 0)
			close(fd);
		case_end("缓冲区写满: 覆盖最老数据并计入 bytes_dropped");
		return;
	}

	fill_pattern(g_a, 4096, 11);	/* A */
	fill_pattern(g_b, 2048, 13);	/* B */

	n = write(fd, g_a, 4096);
	CHECK(n == 4096, "写满 4096 应返回 4096, 实际 %zd (%s)", n, errname());

	n = write(fd, g_b, 2048);
	CHECK(n == 2048, "满缓冲区再写 2048 应返回 2048, 实际 %zd (%s)", n, errname());

	CHECK(ioc_get_stats(fd, &st) == 0, "GET_STATS 失败");
	CHECK(st.bytes_dropped == 2048,
	      "应丢弃 2048 字节未读数据, 实际 %llu", st.bytes_dropped);

	memset(g_c, 0, 4096);
	n = read(fd, g_c, 4096);
	CHECK(n == 4096, "应能读回 4096 字节, 实际 %zd", n);
	CHECK(memcmp(g_c, g_a + 2048, 2048) == 0, "前半应为 A[2048..4095]");
	CHECK(memcmp(g_c + 2048, g_b, 2048) == 0, "后半应为新写入的 B");

	close(fd);
	case_end("缓冲区写满: 覆盖最老未读数据 + bytes_dropped 计数正确");
}

static void t_flag_no_drop(void)
{
	struct lab_config cfg, applied;
	struct lab_stats st;
	ssize_t n;
	int fd;

	case_begin();
	fd = open(g_dev, O_RDWR);
	if (fd < 0 || reset_to(fd, 4096, LAB_FLAG_NO_DROP, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd >= 0)
			close(fd);
		case_end("LAB_FLAG_NO_DROP: 满时返回 -ENOSPC 且不丢数据");
		return;
	}

	fill_pattern(g_a, 3000, 17);
	CHECK(write(fd, g_a, 3000) == 3000, "写 3000 失败: %s", errname());

	errno = 0;
	n = write(fd, g_a, 2000);	/* 只剩 1096 字节空间 */
	CHECK(n == -1 && errno == ENOSPC,
	      "严格模式下空间不足应返回 -ENOSPC, 实际 n=%zd %s", n, errname());

	n = write(fd, g_a, 1000);	/* 刚好放得下 */
	CHECK(n == 1000, "1000 字节应能写入, 实际 %zd (%s)", n, errname());

	errno = 0;
	n = write(fd, g_a, 200);
	CHECK(n == -1 && errno == ENOSPC,
	      "剩余 96 字节空间, 写 200 应 -ENOSPC, 实际 n=%zd %s", n, errname());

	CHECK(ioc_get_stats(fd, &st) == 0, "GET_STATS 失败");
	CHECK(st.bytes_dropped == 0,
	      "严格模式下不应有任何丢弃, 实际 %llu", st.bytes_dropped);
	CHECK(st.bytes_written == 4000,
	      "已写入应为 4000 字节, 实际 %llu", st.bytes_written);

	/* 关掉严格模式后又能覆盖写入了 */
	memset(&cfg, 0, sizeof(cfg));
	cfg.flags = 0;
	CHECK(ioc_set_config(fd, &cfg, &applied) == 0, "关闭 NO_DROP 失败: %s", errname());
	/*
	 * 此时 used=4000、空闲 96 字节。写 200 字节需要覆盖最老的
	 * 200-96 = 104 字节未读数据，这 104 字节要如实计入 bytes_dropped。
	 */
	errno = 0;
	n = write(fd, g_a, 200);
	CHECK(n == 200, "关闭严格模式后写 200 应返回 200, 实际 %zd (%s)", n, errname());
	CHECK(ioc_get_stats(fd, &st) == 0, "GET_STATS 失败");
	CHECK(st.bytes_dropped == 104,
	      "覆盖 104 字节未读数据, bytes_dropped 应为 104, 实际 %llu",
	      st.bytes_dropped);

	CHECK(reset_to(fd, 4096, 0, 0) == 0, "恢复默认配置失败");
	close(fd);
	case_end("LAB_FLAG_NO_DROP: 满时 -ENOSPC, 不丢任何数据");
}

static void t_ioctl_invalid(void)
{
	struct lab_config cfg, applied, before, after;
	unsigned int orig_size;
	long ret;
	int fd;

	case_begin();
	fd = open(g_dev, O_RDWR);
	if (fd < 0 || reset_to(fd, 4096, 0, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd >= 0)
			close(fd);
		case_end("非法 ioctl 命令与非法参数被拒绝");
		return;
	}

	CHECK(ioc_get_config(fd, &before) == 0, "读取初始配置失败");
	orig_size = before.buffer_size;

	/* 1) magic 不对 */
	errno = 0;
	ret = ioctl(fd, _IOR('X', 1, struct lab_config), &cfg);
	CHECK(ret == -1 && errno == ENOTTY,
	      "错误 magic 应返回 -ENOTTY, 实际 ret=%ld %s", ret, errname());

	/* 2) 序号越界 */
	errno = 0;
	ret = ioctl(fd, _IOR(LAB_IOC_MAGIC, 99, struct lab_config), &cfg);
	CHECK(ret == -1 && errno == ENOTTY,
	      "越界序号应返回 -ENOTTY, 实际 ret=%ld %s", ret, errname());

	/* 3) 完全乱来 */
	errno = 0;
	ret = ioctl(fd, 0xdeadbeef, &cfg);
	CHECK(ret == -1 && errno == ENOTTY,
	      "垃圾命令应返回 -ENOTTY, 实际 ret=%ld %s", ret, errname());

	/*
	 * 4) magic/序号都对，但参数大小不对——模拟“用户态头文件和
	 *    内核模块版本不一致”。内核按完整 cmd 匹配，必须拒绝。
	 */
	errno = 0;
	ret = ioctl(fd, _IOR(LAB_IOC_MAGIC, 4, struct lab_config), &cfg);
	CHECK(ret == -1 && errno == ENOTTY,
	      "参数大小不匹配应返回 -ENOTTY, 实际 ret=%ld %s", ret, errname());

	/* 5) 非法 flags */
	memset(&cfg, 0, sizeof(cfg));
	cfg.flags = 0xDEAD0000;
	errno = 0;
	ret = ioc_set_config(fd, &cfg, &applied);
	CHECK(ret == -1 && errno == EINVAL,
	      "未知 flags 位应返回 -EINVAL, 实际 ret=%ld %s", ret, errname());

	/* 6) reserved 非 0 */
	memset(&cfg, 0, sizeof(cfg));
	cfg.reserved = 1;
	errno = 0;
	ret = ioc_set_config(fd, &cfg, &applied);
	CHECK(ret == -1 && errno == EINVAL,
	      "reserved!=0 应返回 -EINVAL, 实际 ret=%ld %s", ret, errname());
	CHECK(ioc_get_config(fd, &after) == 0, "GET_CONFIG 失败");
	CHECK(after.flags == before.flags, "被拒绝的请求不应改动 flags");

	/* 7) buffer_size 太小 / 太大 */
	memset(&cfg, 0, sizeof(cfg));
	cfg.buffer_size = LAB_MIN_BUF - 1;
	errno = 0;
	ret = ioc_set_config(fd, &cfg, &applied);
	CHECK(ret == -1 && errno == EINVAL,
	      "buffer_size 过小应返回 -EINVAL, 实际 ret=%ld %s", ret, errname());

	memset(&cfg, 0, sizeof(cfg));
	cfg.buffer_size = LAB_MAX_BUF + 1;
	errno = 0;
	ret = ioc_set_config(fd, &cfg, &applied);
	CHECK(ret == -1 && errno == EINVAL,
	      "buffer_size 过大应返回 -EINVAL, 实际 ret=%ld %s", ret, errname());

	CHECK(ioc_get_config(fd, &after) == 0, "GET_CONFIG 失败");
	CHECK(after.buffer_size == orig_size,
	      "被拒绝的请求不应改动 buffer_size (%u -> %u)",
	      orig_size, after.buffer_size);

	close(fd);
	case_end("非法 ioctl: 错误 magic/序号/大小/参数全部被拒绝且不改变状态");
}

static void t_bad_user_pointer(void)
{
	void *p;
	ssize_t n;
	int fd;

	case_begin();
	fd = open(g_dev, O_RDWR);
	if (fd < 0 || reset_to(fd, 4096, 0, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd >= 0)
			close(fd);
		case_end("非法用户指针返回 -EFAULT 且数据不丢");
		return;
	}

	/* 拿一页可写内存，再改成 PROT_NONE，制造一个必定缺页的地址 */
	p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(p != MAP_FAILED, "mmap 失败: %s", strerror(errno));
	if (p == MAP_FAILED) {
		close(fd);
		case_end("非法用户指针返回 -EFAULT 且数据不丢");
		return;
	}
	CHECK(mprotect(p, 4096, PROT_NONE) == 0, "mprotect 失败: %s", strerror(errno));

	fill_pattern(g_a, 32, 21);

	/* 写：copy_from_user 应当失败，返回 -EFAULT 而不是让进程崩溃 */
	errno = 0;
	n = write(fd, p, 16);
	CHECK(n == -1 && errno == EFAULT,
	      "非法写指针应返回 -EFAULT, 实际 n=%zd %s", n, errname());

	/* 准备 32 字节数据，然后往非法地址读 */
	CHECK(write(fd, g_a, 32) == 32, "准备数据失败: %s", errname());
	errno = 0;
	n = read(fd, p, 16);
	CHECK(n == -1 && errno == EFAULT,
	      "非法读指针应返回 -EFAULT, 实际 n=%zd %s", n, errname());

	/*
	 * 关键：copy_to_user 失败后驱动必须把读游标回退，
	 * 否则那 16 字节就凭空消失了。
	 */
	memset(g_b, 0, 32);
	n = read(fd, g_b, 32);
	CHECK(n == 32, "回滚后应仍能读满 32 字节, 实际 %zd (%s)", n, errname());
	CHECK(memcmp(g_b, g_a, 32) == 0, "copy_to_user 失败后数据被破坏/丢失");

	munmap(p, 4096);
	close(fd);
	case_end("非法用户指针: copy_*_user 返回 -EFAULT 且读游标回滚");
}

static void t_zero_length(void)
{
	struct lab_stats st;
	ssize_t n;
	int fd;

	case_begin();
	fd = open(g_dev, O_RDWR);
	if (fd < 0 || reset_to(fd, 4096, 0, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd >= 0)
			close(fd);
		case_end("count==0 的读写返回 0");
		return;
	}

	n = write(fd, g_a, 0);
	CHECK(n == 0, "write 0 字节应返回 0, 实际 %zd (%s)", n, errname());
	n = read(fd, g_b, 0);
	CHECK(n == 0, "read 0 字节应返回 0, 实际 %zd (%s)", n, errname());

	CHECK(ioc_get_stats(fd, &st) == 0, "GET_STATS 失败");
	CHECK(st.bytes_written == 0 && st.bytes_read == 0,
	      "0 字节读写不应改变统计 (written=%llu read=%llu)",
	      st.bytes_written, st.bytes_read);

	close(fd);
	case_end("count == 0 的 read/write 返回 0");
}

static void t_huge_write(void)
{
	struct lab_stats st;
	ssize_t n;
	int fd;

	case_begin();
	fd = open(g_dev, O_RDWR);
	if (fd < 0 || reset_to(fd, 4096, 0, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd >= 0)
			close(fd);
		case_end("超大写入被截断为缓冲区容量(短写)");
		return;
	}

	fill_pattern(g_a, 1 << 20, 23);	/* 1 MiB */
	errno = 0;
	n = write(fd, g_a, 1 << 20);
	CHECK(n == 4096,
	      "写 1 MiB 到 4 KiB 缓冲区应短写为 4096, 实际 %zd (%s)", n, errname());

	CHECK(ioc_get_stats(fd, &st) == 0, "GET_STATS 失败");
	CHECK(st.bytes_written == 4096, "bytes_written 应为 4096, 实际 %llu",
	      st.bytes_written);

	/* 空缓冲区上被截断写入不应产生丢弃 */
	CHECK(st.bytes_dropped == 0, "空缓冲区不应有丢弃, 实际 %llu", st.bytes_dropped);

	memset(g_b, 0, 4096);
	n = read(fd, g_b, 4096);
	CHECK(n == 4096, "应读回 4096 字节, 实际 %zd", n);
	CHECK(memcmp(g_b, g_a, 4096) == 0, "读回内容应为源数据的前 4096 字节");

	/* 反向：超大的 count 也只返回实际可读的字节数 */
	CHECK(write(fd, g_a, 10) == 10, "准备数据失败: %s", errname());
	memset(g_b, 0, 64);
	n = read(fd, g_b, 1 << 20);
	CHECK(n == 10, "count 远大于可用数据时应只返回 10, 实际 %zd (%s)", n, errname());

	close(fd);
	case_end("超大写入/读取: 短写、不丢数据、符合边界约定");
}

static void t_nonblock_write(void)
{
	ssize_t n;
	int fd;

	case_begin();
	fd = open(g_dev, O_RDWR | O_NONBLOCK);
	if (fd < 0 || reset_to(fd, 4096, 0, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd >= 0)
			close(fd);
		case_end("O_NONBLOCK 下写入永不阻塞");
		return;
	}

	fill_pattern(g_a, 4096, 27);
	n = write(fd, g_a, 4096);
	CHECK(n == 4096, "非阻塞写 4096 应成功, 实际 %zd (%s)", n, errname());
	n = write(fd, g_a, 4096);
	CHECK(n == 4096, "非阻塞满缓冲区写应成功(覆盖), 实际 %zd (%s)", n, errname());

	close(fd);
	case_end("O_NONBLOCK: 写入永不阻塞, 满时覆盖");
}

static void t_state_across_close(void)
{
	ssize_t n;
	int fd;

	case_begin();
	fd = open(g_dev, O_RDWR);
	if (fd < 0 || reset_to(fd, 4096, 0, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd >= 0)
			close(fd);
		case_end("缓冲区是设备级的: 关闭再打开数据仍在");
		return;
	}

	fill_pattern(g_a, 64, 29);
	CHECK(write(fd, g_a, 64) == 64, "write 失败: %s", errname());
	close(fd);

	fd = open(g_dev, O_RDWR);
	CHECK(fd >= 0, "重新打开失败: %s", strerror(errno));
	if (fd >= 0) {
		memset(g_b, 0, 64);
		n = read(fd, g_b, 64);
		CHECK(n == 64, "重新打开后应读到 64 字节, 实际 %zd (%s)", n, errname());
		CHECK(memcmp(g_a, g_b, 64) == 0, "跨 open/close 的数据不一致");
		close(fd);
	}

	case_end("缓冲区是设备级状态: close 后重新 open 数据仍在");
}

static void t_multi_fd(void)
{
	ssize_t n;
	int fd1, fd2;

	case_begin();
	fd1 = open(g_dev, O_RDWR);
	fd2 = open(g_dev, O_RDWR);
	if (fd1 < 0 || fd2 < 0 || reset_to(fd1, 4096, 0, 0) < 0) {
		CHECK(0, "设备准备失败");
		if (fd1 >= 0)
			close(fd1);
		if (fd2 >= 0)
			close(fd2);
		case_end("多个 fd 共享同一个设备缓冲区");
		return;
	}

	fill_pattern(g_a, 128, 31);
	CHECK(write(fd1, g_a, 128) == 128, "fd1 写入失败: %s", errname());
	memset(g_b, 0, 128);
	n = read(fd2, g_b, 128);
	CHECK(n == 128, "fd2 应读到 128 字节, 实际 %zd (%s)", n, errname());
	CHECK(memcmp(g_a, g_b, 128) == 0, "两个 fd 之间数据不一致");

	close(fd1);
	close(fd2);
	case_end("多个 fd 共享同一缓冲区 (设备级 FIFO 语义)");
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	if (argc > 1)
		g_dev = argv[1];

	printf("=== labdev 用户态功能测试 (真实内核态, 需要已 insmod) ===\n");
	printf("设备: %s\n\n", g_dev);

	if (access(g_dev, F_OK) != 0) {
		fprintf(stderr, "错误: %s 不存在，请先 sudo insmod labdev.ko\n", g_dev);
		return 1;
	}

	t_open_and_defaults();
	t_roundtrip();
	t_nonblock_empty_read();
	t_blocking_read_wakeup();
	t_poll_semantics();
	t_epoll_level_triggered();
	t_llseek();
	t_ioctl_config_and_chunk();
	t_ioctl_resize();
	t_buffer_full_and_drop();
	t_flag_no_drop();
	t_ioctl_invalid();
	t_bad_user_pointer();
	t_zero_length();
	t_huge_write();
	t_nonblock_write();
	t_state_across_close();
	t_multi_fd();

	printf("\n用例: %d 个, 失败 %d 个 | 断言: %d 次\n",
	       g_cases, g_cases_failed, g_checks);

	if (g_cases_failed) {
		printf("结果: FAIL\n");
		return 1;
	}
	printf("结果: PASS\n");
	return 0;
}
