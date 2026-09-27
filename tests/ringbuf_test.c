// SPDX-License-Identifier: MIT
/*
 * ringbuf_test.c - 环形缓冲区核心逻辑的用户态单元测试
 *
 * 重要说明（诚实性）：
 *   这是**用户态**测试，测的是 ringbuf.c 这份算法实现，不是内核态测试。
 *   内核模块 labdev.ko 链接的是同一份 ringbuf.c，所以这里通过就意味着
 *   模块里的缓冲区逻辑是对的；但锁、等待队列、copy_*_user 这些
 *   只有真正 insmod 之后才能验证（见 tools/dtest.c 和 CI）。
 *
 * 测试分两类：
 *   1) 确定性用例：逐条覆盖边界行为
 *   2) 随机模糊测试：用一个“参考模型”（动态数组 + 读游标）做逐字节比对，
 *      几十万次随机操作后状态必须完全一致
 *
 * 退出码：0 = 全部通过，1 = 有失败。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ringbuf.h"

static int g_checks;
static int g_case_failed;
static int g_cases;
static int g_cases_failed;

#define ASSERT(cond, ...)						\
	do {								\
		g_checks++;						\
		if (!(cond)) {						\
			g_case_failed = 1;				\
			printf("      ! 断言失败: ");			\
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

/* 把缓冲区里“还能读出来”的字节按顺序拷出来，便于逐字节比对 */
static void rb_avail_bytes(const struct ringbuf *rb, unsigned char *out)
{
	size_t i, n = ringbuf_avail(rb);

	for (i = 0; i < n; i++)
		out[i] = rb->buf[(rb->rpos + i) % rb->size];
}

/* ------------------------------------------------------------------ */
/* 参考模型：一个普通动态数组 + 读游标，语义与 ringbuf 完全对应          */
/* ------------------------------------------------------------------ */

struct model {
	unsigned char *data;	/* 缓冲区里的全部有效字节 */
	size_t len;		/* 有效字节数 */
	size_t cap;
	size_t pos;		/* 读游标 */
	size_t size;		/* 模拟的缓冲区容量 */
	size_t dropped;		/* 累计被覆盖丢弃的未读字节数 */
};

static void model_init(struct model *m, size_t size)
{
	m->cap = size + 8;
	m->data = malloc(m->cap);
	if (!m->data) {
		fprintf(stderr, "malloc 失败\n");
		exit(2);
	}
	m->len = 0;
	m->pos = 0;
	m->size = size;
	m->dropped = 0;
}

static void model_free(struct model *m)
{
	free(m->data);
	m->data = NULL;
}

static size_t model_write(struct model *m, const unsigned char *src, size_t len,
			  size_t *dropped)
{
	size_t space, accepted, d;

	if (dropped)
		*dropped = 0;
	if (len == 0)
		return 0;

	space = m->size - m->len;	/* 与实现一致：先只算空闲，不算可回收 */

	if (len > space && m->pos > 0) {
		space += m->pos;
		memmove(m->data, m->data + m->pos, m->len - m->pos);
		m->len -= m->pos;
		m->pos = 0;
	}

	if (len > space) {
		d = len - space;
		if (d > m->len)
			d = m->len;
		memmove(m->data, m->data + d, m->len - d);
		m->len -= d;
		space += d;
		m->dropped += d;
		if (dropped)
			*dropped = d;
	}

	accepted = (len < space) ? len : space;
	memcpy(m->data + m->len, src, accepted);
	m->len += accepted;

	return accepted;
}

static size_t model_read(struct model *m, unsigned char *dst, size_t len)
{
	size_t n;

	n = m->len - m->pos;
	if (len < n)
		n = len;

	memcpy(dst, m->data + m->pos, n);
	m->pos += n;

	/* 与实现保持一致：读空时**不**归零游标和数据，数据一直保留到被覆盖 */
	return n;
}

static long model_seek(struct model *m, long offset, int whence)
{
	long base, pos;

	switch (whence) {
	case RB_SEEK_SET:
		base = 0;
		break;
	case RB_SEEK_CUR:
		base = (long)m->pos;
		break;
	case RB_SEEK_END:
		base = (long)m->len;
		break;
	default:
		return -1;
	}

	pos = base + offset;
	if (pos < 0 || pos > (long)m->len)
		return -1;

	m->pos = (size_t)pos;
	return pos;
}

/* ------------------------------------------------------------------ */
/* 确定性用例                                                           */
/* ------------------------------------------------------------------ */

#define BIGBUF 4096

static unsigned char g_mem[BIGBUF];
static unsigned char g_out[BIGBUF];

static void t_setup_and_reset(void)
{
	struct ringbuf rb;

	case_begin();
	ringbuf_setup(&rb, g_mem, 256);
	ASSERT(ringbuf_size(&rb) == 256, "容量应为 256, 实际 %zu", ringbuf_size(&rb));
	ASSERT(ringbuf_used(&rb) == 0, "used 应为 0");
	ASSERT(ringbuf_avail(&rb) == 0, "avail 应为 0");
	ASSERT(ringbuf_space(&rb) == 256, "空闲空间应为 256, 实际 %zu", ringbuf_space(&rb));

	ringbuf_write(&rb, (const unsigned char *)"abc", 3, NULL);
	ASSERT(ringbuf_used(&rb) == 3, "写 3 字节后 used 应为 3");
	ringbuf_reset(&rb);
	ASSERT(ringbuf_used(&rb) == 0 && ringbuf_avail(&rb) == 0 &&
	       ringbuf_space(&rb) == 256, "reset 后应回到初始状态");
	case_end("setup / reset 初始状态");
}

static void t_linear_roundtrip(void)
{
	struct ringbuf rb;
	unsigned char in[100], out[100];
	size_t i;

	case_begin();
	for (i = 0; i < sizeof(in); i++)
		in[i] = (unsigned char)(i * 7 + 1);

	ringbuf_setup(&rb, g_mem, 256);
	ASSERT(ringbuf_write(&rb, in, sizeof(in), NULL) == sizeof(in), "应接受全部 100 字节");
	ASSERT(ringbuf_avail(&rb) == 100, "avail 应为 100");
	memset(out, 0, sizeof(out));
	ASSERT(ringbuf_read(&rb, out, sizeof(out)) == sizeof(out), "应读出 100 字节");
	ASSERT(memcmp(in, out, sizeof(in)) == 0, "读出内容应与写入一致");
	ASSERT(ringbuf_avail(&rb) == 0, "读完后 avail 应为 0");
	case_end("线性写入/读出数据一致");
}

static void t_wraparound(void)
{
	struct ringbuf rb;
	unsigned char in[64], out[400];
	size_t i, read_total = 0;

	case_begin();
	for (i = 0; i < sizeof(in); i++)
		in[i] = (unsigned char)i;

	ringbuf_setup(&rb, g_mem, 64);

	/*
	 * 反复“写入 50 字节—读空”，每轮都把 head 往前推 50，
	 * 于是 head/rpos 必然跨过缓冲区末尾绕回开头（64 不整除 50）。
	 */
	for (i = 0; i < 8; i++) {
		ASSERT(ringbuf_write(&rb, in, 50, NULL) == 50,
		       "第 %zu 轮应接受 50 字节", i);
		ASSERT(ringbuf_read(&rb, out + read_total, 50) == 50,
		       "第 %zu 轮应读出 50 字节", i);
		read_total += 50;
	}
	ASSERT(read_total == 400, "累计读出应为 400 字节, 实际 %zu", read_total);

	/* 每轮读出的都应当是同一段 in[0..49] */
	for (i = 0; i < 8; i++)
		ASSERT(memcmp(out + i * 50, in, 50) == 0,
		       "第 %zu 轮数据错乱（环绕时跨段拷贝有 bug）", i);
	case_end("环绕(wrap)写入/读出");
}

static void t_overflow_drop_oldest(void)
{
	struct ringbuf rb;
	unsigned char a[64], b[32], out[64];
	size_t dropped = 12345;
	size_t i;

	case_begin();
	for (i = 0; i < sizeof(a); i++)
		a[i] = (unsigned char)(0xA0 + i);
	for (i = 0; i < sizeof(b); i++)
		b[i] = (unsigned char)(0xB0 + i);

	ringbuf_setup(&rb, g_mem, 64);
	ASSERT(ringbuf_write(&rb, a, 64, NULL) == 64, "写满 64 字节");
	ASSERT(ringbuf_space(&rb) == 0, "满时空间应为 0");

	ASSERT(ringbuf_write(&rb, b, 32, &dropped) == 32, "满时再写 32 字节应全部接受");
	ASSERT(dropped == 32, "应丢弃 32 字节未读数据, 实际 %zu", dropped);
	ASSERT(ringbuf_used(&rb) == 64, "used 应仍为 64");

	memset(out, 0, sizeof(out));
	ASSERT(ringbuf_read(&rb, out, 64) == 64, "应读出 64 字节");
	ASSERT(memcmp(out, a + 32, 32) == 0, "前半应是 a[32..63]（最老数据被覆盖）");
	ASSERT(memcmp(out + 32, b, 32) == 0, "后半应是新写入的 b");
	case_end("缓冲区满时覆盖最老数据 + dropped 计数");
}

static void t_write_larger_than_capacity(void)
{
	struct ringbuf rb;
	unsigned char out[128];
	size_t dropped = 0;

	case_begin();
	memset(g_mem, 0, BIGBUF);
	memset(g_out, 0xEE, sizeof(g_out));
	ringbuf_setup(&rb, g_mem, 64);

	/* 请求 128 字节但容量只有 64：按约定只接受前 64 字节 */
	ASSERT(ringbuf_write(&rb, g_out, 128, &dropped) == 64,
	       "超过容量的写入应被截断为 64");
	ASSERT(dropped == 0, "空缓冲区上不应有丢弃");
	memset(out, 0, sizeof(out));
	ASSERT(ringbuf_read(&rb, out, 128) == 64, "只能读到 64 字节");
	ASSERT(memcmp(out, g_out, 64) == 0, "内容应为源数据前 64 字节");
	case_end("单次写入超过容量时截断");
}

static void t_read_empty_and_rollback(void)
{
	struct ringbuf rb;
	unsigned char out[16];
	size_t i;

	case_begin();
	for (i = 0; i < 10; i++)
		g_out[i] = (unsigned char)(i + 1);

	ringbuf_setup(&rb, g_mem, 64);
	ASSERT(ringbuf_read(&rb, out, sizeof(out)) == 0, "空缓冲区读应返回 0");
	ASSERT(ringbuf_write(&rb, g_out, 10, NULL) == 10, "写 10 字节");
	ASSERT(ringbuf_read(&rb, out, 4) == 4, "先读 4 字节");
	/* 模拟驱动里 copy_to_user 失败后的回滚（SEEK_CUR 负偏移） */
	ASSERT(ringbuf_seek(&rb, -4, RB_SEEK_CUR) == 0, "负偏移回退应回到 0");
	ASSERT(ringbuf_read(&rb, out, 10) == 10, "回退后应能重新读满 10 字节");
	ASSERT(memcmp(out, g_out, 10) == 0, "回退重读的数据应与原始一致");
	case_end("空读返回 0 + 读游标回滚");
}

static void t_reclaim_consumed_space(void)
{
	struct ringbuf rb;
	unsigned char in[64], out[64];
	size_t dropped = 999;
	size_t i;

	case_begin();
	for (i = 0; i < 64; i++)
		in[i] = (unsigned char)(0xC0 + i);

	ringbuf_setup(&rb, g_mem, 64);
	ringbuf_write(&rb, in, 64, NULL);
	ASSERT(ringbuf_read(&rb, out, 32) == 32, "先读走 32 字节");
	ASSERT(ringbuf_space(&rb) == 32, "已读的 32 字节应算作可用空间(可无损回收)");

	/* 写 32 字节：正好能靠回收“已读字节”腾出来，不应丢弃任何未读数据 */
	ASSERT(ringbuf_write(&rb, in, 32, &dropped) == 32, "应能写入 32 字节");
	ASSERT(dropped == 0, "回收即可满足，不应丢弃数据, 实际 %zu", dropped);

	ASSERT(ringbuf_read(&rb, out, 32) == 32, "读出剩下的 32 字节");
	ASSERT(memcmp(out, in + 32, 32) == 0, "未读数据应完好无损");
	ASSERT(ringbuf_read(&rb, out, 32) == 32, "再读出刚写入的 32 字节");
	ASSERT(memcmp(out, in, 32) == 0, "新写入的数据应正确");
	case_end("回收已读空间且不丢失未读数据");
}

static void t_seek_semantics(void)
{
	struct ringbuf rb;
	unsigned char in[100], out[100];
	size_t i;

	case_begin();
	for (i = 0; i < 100; i++)
		in[i] = (unsigned char)i;

	ringbuf_setup(&rb, g_mem, 256);
	ringbuf_write(&rb, in, 100, NULL);

	ASSERT(ringbuf_seek(&rb, 0, RB_SEEK_SET) == 0, "SEEK_SET 0 应返回 0");
	ASSERT(ringbuf_seek(&rb, 0, RB_SEEK_END) == 100, "SEEK_END 0 应返回 100");
	ASSERT(ringbuf_seek(&rb, 50, RB_SEEK_SET) == 50, "SEEK_SET 50 应返回 50");
	ASSERT(ringbuf_read(&rb, out, 10) == 10, "从 50 处读 10 字节");
	ASSERT(memcmp(out, in + 50, 10) == 0, "读到的是 in[50..59]");
	ASSERT(ringbuf_seek(&rb, 0, RB_SEEK_CUR) == 60, "SEEK_CUR 0 应返回 60");
	ASSERT(ringbuf_seek(&rb, -60, RB_SEEK_CUR) == 0, "SEEK_CUR -60 应回到 0");

	ASSERT(ringbuf_seek(&rb, 101, RB_SEEK_SET) == -1, "超出末尾应失败");
	ASSERT(ringbuf_seek(&rb, -1, RB_SEEK_SET) == -1, "负位置应失败");
	ASSERT(ringbuf_seek(&rb, 0, 99) == -1, "非法 whence 应失败");

	ringbuf_read(&rb, out, 100);
	ASSERT(ringbuf_avail(&rb) == 0, "读空后 avail 为 0");
	case_end("llseek 语义与越界拒绝");
}

static void t_seek_end_then_read(void)
{
	struct ringbuf rb;
	unsigned char in[20], out[20];

	case_begin();
	memset(in, 0x5A, sizeof(in));
	ringbuf_setup(&rb, g_mem, 64);
	ringbuf_write(&rb, in, 20, NULL);
	ASSERT(ringbuf_seek(&rb, 0, RB_SEEK_END) == 20, "游标移到末尾");
	ASSERT(ringbuf_avail(&rb) == 0, "末尾处没有可读数据");
	ASSERT(ringbuf_read(&rb, out, 20) == 0, "末尾读应返回 0");
	ASSERT(ringbuf_used(&rb) == 20, "seek 到末尾不应清空缓冲区");
	ASSERT(ringbuf_seek(&rb, 0, RB_SEEK_SET) == 0, "还能回退到开头");
	ASSERT(ringbuf_read(&rb, out, 20) == 20, "回退后能读满");
	case_end("seek 到末尾后仍可回退重读");
}

static void t_transfer_grow(void)
{
	struct ringbuf src, dst;
	unsigned char in[600], out[600];
	size_t i;

	case_begin();
	for (i = 0; i < 600; i++)
		in[i] = (unsigned char)(i & 0xFF);

	ringbuf_setup(&src, g_mem, 1024);
	ringbuf_setup(&dst, g_mem + 2048, 1024);
	ringbuf_write(&src, in, 600, NULL);
	ASSERT(ringbuf_avail(&src) == 600, "源有 600 字节");

	ASSERT(ringbuf_transfer(&dst, &src) == 600, "应搬运 600 字节");
	ASSERT(ringbuf_avail(&src) == 0, "源应被清空");
	ASSERT(ringbuf_avail(&dst) == 600, "目标应有 600 字节");
	memset(out, 0, sizeof(out));
	ASSERT(ringbuf_read(&dst, out, 600) == 600, "目标应能读出 600 字节");
	ASSERT(memcmp(out, in, 600) == 0, "搬运后数据顺序应完全一致");
	case_end("transfer 到更大缓冲区: 数据完整保留");
}

static void t_transfer_shrink(void)
{
	struct ringbuf src, dst;
	unsigned char in[600], out[600];
	size_t i;

	case_begin();
	for (i = 0; i < 600; i++)
		in[i] = (unsigned char)(i & 0xFF);

	ringbuf_setup(&src, g_mem, 1024);
	ringbuf_setup(&dst, g_mem + 2048, 256);
	ringbuf_write(&src, in, 600, NULL);

	ASSERT(ringbuf_transfer(&dst, &src) == 256, "目标只有 256 字节, 应搬运 256");
	ASSERT(ringbuf_avail(&src) == 0, "源应被清空");
	memset(out, 0, sizeof(out));
	ASSERT(ringbuf_read(&dst, out, 256) == 256, "目标可读出 256");
	ASSERT(memcmp(out, in, 256) == 0, "保留的应是最老的 256 字节");
	case_end("transfer 到更小缓冲区: 保留前若干字节");
}

static void t_transfer_only_unread(void)
{
	struct ringbuf src, dst;
	unsigned char in[600], out[600];
	size_t i;

	case_begin();
	for (i = 0; i < 600; i++)
		in[i] = (unsigned char)(i & 0xFF);

	ringbuf_setup(&src, g_mem, 1024);
	ringbuf_setup(&dst, g_mem + 2048, 1024);
	ringbuf_write(&src, in, 600, NULL);
	ASSERT(ringbuf_read(&src, out, 100) == 100, "先读走 100 字节");

	ASSERT(ringbuf_transfer(&dst, &src) == 500, "只应搬运未读的 500 字节");
	ASSERT(ringbuf_read(&dst, out, 500) == 500, "目标读出 500");
	ASSERT(memcmp(out, in + 100, 500) == 0, "搬运的是 in[100..599]");
	case_end("transfer 只搬运未读数据");
}

static void t_degenerate_size(void)
{
	struct ringbuf rb;
	unsigned char out[8];

	case_begin();
	/* size < 2 视为非法：所有操作安全返回 0/-1，绝不越界 */
	ringbuf_setup(&rb, g_mem, 0);
	ASSERT(ringbuf_size(&rb) == 0, "容量应为 0");
	ASSERT(ringbuf_write(&rb, out, 4, NULL) == 0, "非法缓冲区写入应返回 0");
	ASSERT(ringbuf_read(&rb, out, 4) == 0, "非法缓冲区读取应返回 0");
	ASSERT(ringbuf_space(&rb) == 0, "非法缓冲区空间应为 0");
	ASSERT(ringbuf_seek(&rb, 0, RB_SEEK_SET) == -1, "非法缓冲区 seek 应失败");

	ringbuf_setup(&rb, g_mem, 1);
	ASSERT(ringbuf_size(&rb) == 0, "size=1 同样视为非法");
	case_end("退化容量(size<2)安全降级");
}

static void t_zero_length_ops(void)
{
	struct ringbuf rb;
	unsigned char out[8];

	case_begin();
	ringbuf_setup(&rb, g_mem, 64);
	ASSERT(ringbuf_write(&rb, out, 0, NULL) == 0, "写 0 字节返回 0");
	ASSERT(ringbuf_read(&rb, out, 0) == 0, "读 0 字节返回 0");
	ASSERT(ringbuf_used(&rb) == 0, "状态不变");
	case_end("零长度读写");
}

/* ------------------------------------------------------------------ */
/* 随机模糊测试：与参考模型逐字节比对                                    */
/* ------------------------------------------------------------------ */

struct fuzz_ctx {
	struct ringbuf rb;
	struct model m;
	unsigned char mem[1024];
	unsigned char tmp[4096];
	unsigned char want[2048];
	unsigned char got[2048];
	unsigned long seed;
	size_t size;
};

static unsigned long xrand(unsigned long *s)
{
	/* xorshift64*：确定性、跨平台一致，失败可以靠 seed 复现 */
	unsigned long x = *s;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*s = x;
	return (x * 0x2545F4914F6CDD1DUL) >> 16;
}

static int fuzz_check(struct fuzz_ctx *c, const char *stage, int op)
{
	size_t na, nm;

	na = ringbuf_avail(&c->rb);
	nm = c->m.len - c->m.pos;

	if (na != nm) {
		printf("      ! [%s] op=%d avail 不一致: rb=%zu model=%zu\n",
		       stage, op, na, nm);
		return 1;
	}
	if (ringbuf_used(&c->rb) != c->m.len) {
		printf("      ! [%s] op=%d used 不一致: rb=%zu model=%zu\n",
		       stage, op, ringbuf_used(&c->rb), c->m.len);
		return 1;
	}
	if (ringbuf_used(&c->rb) > ringbuf_size(&c->rb)) {
		printf("      ! [%s] op=%d used 超过容量\n", stage, op);
		return 1;
	}
	if (na > 0) {
		rb_avail_bytes(&c->rb, c->got);
		if (memcmp(c->got, c->m.data + c->m.pos, na) != 0) {
			printf("      ! [%s] op=%d 可读数据内容不一致 (%zu 字节)\n",
			       stage, op, na);
			return 1;
		}
	}
	return 0;
}

static void t_fuzz(size_t size, unsigned long seed, int iters)
{
	struct fuzz_ctx *c;
	int i, bad = 0;
	char name[128];

	c = calloc(1, sizeof(*c));
	if (!c) {
		fprintf(stderr, "calloc 失败\n");
		exit(2);
	}
	c->seed = seed ? seed : 1;
	c->size = size;

	case_begin();
	ringbuf_setup(&c->rb, c->mem, size);
	model_init(&c->m, size);

	for (i = 0; i < iters && !bad; i++) {
		unsigned long r = xrand(&c->seed);
		int op = (int)(r % 100);
		size_t n, k;
		long off;
		int whence;

		/* 单次操作最多用到 2*size+1 字节，不必把整个 tmp 都填满 */
		for (k = 0; k < 2 * size + 2; k++)
			c->tmp[k] = (unsigned char)xrand(&c->seed);

		if (op < 45) {			/* 写 */
			size_t d1 = 0, d2 = 0;
			size_t want = (size_t)(xrand(&c->seed) % (2 * size + 1));

			n = ringbuf_write(&c->rb, c->tmp, want, &d1);
			k = model_write(&c->m, c->tmp, want, &d2);
			if (n != k || d1 != d2) {
				printf("      ! [fuzz] i=%d 写入结果不一致: "
				       "rb(%zu,d%zu) model(%zu,d%zu) want=%zu\n",
				       i, n, d1, k, d2, want);
				bad = 1;
				break;
			}
		} else if (op < 80) {		/* 读 */
			size_t want = (size_t)(xrand(&c->seed) % (2 * size + 1));

			n = ringbuf_read(&c->rb, c->got, want);
			k = model_read(&c->m, c->want, want);
			if (n != k) {
				printf("      ! [fuzz] i=%d 读出长度不一致: "
				       "rb=%zu model=%zu\n", i, n, k);
				bad = 1;
				break;
			}
			if (n > 0 && memcmp(c->got, c->want, n) != 0) {
				printf("      ! [fuzz] i=%d 读出内容不一致 (%zu 字节)\n",
				       i, n);
				bad = 1;
				break;
			}
		} else if (op < 95) {		/* seek */
			long lo = -(long)(2 * size);
			long rng = (long)(4 * size) + 1;

			whence = (int)(xrand(&c->seed) % 3);
			off = lo + (long)(xrand(&c->seed) % (unsigned long)rng);
			if (xrand(&c->seed) % 16 == 0)
				whence = 99;	/* 偶尔试试非法 whence */

			{
				long r1 = ringbuf_seek(&c->rb, off, whence);
				long r2 = model_seek(&c->m, off, whence);

				if (r1 != r2) {
					printf("      ! [fuzz] i=%d seek 结果不一致: "
					       "rb=%ld model=%ld (off=%ld whence=%d)\n",
					       i, r1, r2, off, whence);
					bad = 1;
					break;
				}
			}
		} else {			/* reset */
			ringbuf_reset(&c->rb);
			c->m.len = 0;
			c->m.pos = 0;
		}

		if (fuzz_check(c, "fuzz", i)) {
			bad = 1;
			break;
		}
	}

	ASSERT(!bad, "模糊测试在 %d 次操作内出现状态分歧 (size=%zu seed=%lu)",
	       iters, size, seed);

	model_free(&c->m);
	free(c);

	snprintf(name, sizeof(name),
		 "模糊测试: 容量 %zu, %d 次随机操作, 与参考模型逐字节一致",
		 size, iters);
	case_end(name);
}

/* ------------------------------------------------------------------ */

int main(void)
{
	printf("=== ringbuf 用户态单元测试 (内核模块链接的是同一份 ringbuf.c) ===\n\n");

	t_setup_and_reset();
	t_linear_roundtrip();
	t_wraparound();
	t_overflow_drop_oldest();
	t_write_larger_than_capacity();
	t_read_empty_and_rollback();
	t_reclaim_consumed_space();
	t_seek_semantics();
	t_seek_end_then_read();
	t_transfer_grow();
	t_transfer_shrink();
	t_transfer_only_unread();
	t_degenerate_size();
	t_zero_length_ops();

	t_fuzz(8, 0x1234ABCDUL, 50000);
	t_fuzz(64, 0x0BADF00DUL, 100000);
	t_fuzz(257, 0xFEEDFACEUL, 100000);

	printf("\n用例: %d 个, 失败 %d 个 | 断言: %d 次\n",
	       g_cases, g_cases_failed, g_checks);

	if (g_cases_failed) {
		printf("结果: FAIL\n");
		return 1;
	}
	printf("结果: PASS\n");
	return 0;
}
