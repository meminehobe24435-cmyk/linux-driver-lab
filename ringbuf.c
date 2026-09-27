// SPDX-License-Identifier: MIT
/*
 * ringbuf.c - 环形缓冲区核心实现（内核态/用户态共用）
 *
 * 全部操作都是纯内存读写，不含任何锁与原子操作，因此可以被内核模块和
 * 用户态单元测试以完全相同的方式调用。
 */
#include "ringbuf.h"

#ifdef __KERNEL__
#include <linux/string.h>	/* memcpy/memset */
#include <linux/stddef.h>	/* NULL */
#else
#include <string.h>
#endif

void ringbuf_setup(struct ringbuf *rb, unsigned char *mem, size_t size)
{
	if (!rb)
		return;

	rb->buf = mem;
	/* 空/满无法区分的老问题：本实现显式维护 used，因此不需要牺牲一个字节 */
	rb->size = (mem && size >= 2) ? size : 0;
	rb->head = 0;
	rb->tail = 0;
	rb->rpos = 0;
	rb->used = 0;
	rb->consumed = 0;
}

void ringbuf_reset(struct ringbuf *rb)
{
	if (!rb)
		return;

	rb->head = 0;
	rb->tail = 0;
	rb->rpos = 0;
	rb->used = 0;
	rb->consumed = 0;
}

size_t ringbuf_size(const struct ringbuf *rb)
{
	return rb ? rb->size : 0;
}

size_t ringbuf_used(const struct ringbuf *rb)
{
	return rb ? rb->used : 0;
}

size_t ringbuf_avail(const struct ringbuf *rb)
{
	if (!rb)
		return 0;
	return rb->used - rb->consumed;
}

size_t ringbuf_space(const struct ringbuf *rb)
{
	if (!rb || rb->size == 0)
		return 0;
	/*
	 * 读游标已经越过的字节（consumed）可以无损回收，
	 * 所以它们也算“可用空间”。
	 */
	return rb->size - rb->used + rb->consumed;
}

size_t ringbuf_write(struct ringbuf *rb, const unsigned char *src, size_t len,
		     size_t *dropped)
{
	size_t space, accepted, first;
	size_t lost = 0;

	if (dropped)
		*dropped = 0;

	if (!rb || rb->size == 0 || len == 0)
		return 0;

	space = rb->size - rb->used;

	/*
	 * 第一步：空间不够时回收“读过但还没被复用”的字节。
	 * 这一步不丢任何未读数据；副作用是 llseek 的回退能力会失效
	 * （那些字节马上会被覆盖，退回去只会读到新数据），这是符合直觉的。
	 */
	if (len > space && rb->consumed > 0) {
		space += rb->consumed;
		rb->used -= rb->consumed;
		rb->tail = rb->rpos;
		rb->consumed = 0;
	}

	/*
	 * 第二步：仍然不够，只能覆盖最老的未读数据。
	 * 经过第一步后 consumed 必为 0，所以这里丢掉的每一个字节
	 * 都是用户还没读走的，可以如实计入 dropped。
	 */
	if (len > space) {
		size_t drop = len - space;

		if (drop > rb->used)
			drop = rb->used;

		rb->tail = (rb->tail + drop) % rb->size;
		/* 读游标原本就停在 tail 上，必须跟着一起前移 */
		rb->rpos = (rb->rpos + drop) % rb->size;
		rb->used -= drop;
		space += drop;
		lost = drop;
	}

	accepted = (len < space) ? len : space;
	if (accepted == 0)
		return 0;

	/* 环形内存可能在末尾断开，分两段拷贝 */
	first = rb->size - rb->head;
	if (first > accepted)
		first = accepted;
	memcpy(rb->buf + rb->head, src, first);
	if (accepted > first)
		memcpy(rb->buf, src + first, accepted - first);

	rb->head = (rb->head + accepted) % rb->size;
	rb->used += accepted;

	if (dropped)
		*dropped = lost;

	return accepted;
}

size_t ringbuf_read(struct ringbuf *rb, unsigned char *dst, size_t len)
{
	size_t avail, first;

	if (!rb || rb->size == 0 || len == 0)
		return 0;

	avail = rb->used - rb->consumed;
	if (len > avail)
		len = avail;
	if (len == 0)
		return 0;

	first = rb->size - rb->rpos;
	if (first > len)
		first = len;
	memcpy(dst, rb->buf + rb->rpos, first);
	if (len > first)
		memcpy(dst + first, rb->buf, len - first);

	rb->rpos = (rb->rpos + len) % rb->size;
	rb->consumed += len;

	/*
	 * 这里**故意不**在“刚好读空”时把游标归零。
	 * 归零会让 llseek 的回退能力变得时有时无：
	 *   写 100、读 50、回退 -> 还能重读 100 字节；
	 *   写 100、读 100、回退 -> 却什么都没有了。
	 * 统一成“数据一直可读，直到被新数据覆盖”之后语义就一致了，
	 * 代价只是 used 会保持原值（avail 仍为 0），下次写入时由
	 * ringbuf_write() 的回收逻辑把这部分空间无损拿回来。
	 */

	return len;
}

long ringbuf_seek(struct ringbuf *rb, long offset, int whence)
{
	long base, pos;

	if (!rb || rb->size == 0)
		return -1;

	switch (whence) {
	case RB_SEEK_SET:
		base = 0;
		break;
	case RB_SEEK_CUR:
		base = (long)rb->consumed;
		break;
	case RB_SEEK_END:
		base = (long)rb->used;
		break;
	default:
		return -1;
	}

	pos = base + offset;
	/* 只能停留在“缓冲区里还有的数据”范围内 */
	if (pos < 0 || pos > (long)rb->used)
		return -1;

	rb->consumed = (size_t)pos;
	rb->rpos = (rb->tail + (size_t)pos) % rb->size;

	return pos;
}

size_t ringbuf_transfer(struct ringbuf *dst, struct ringbuf *src)
{
	size_t avail, total = 0;

	if (!dst || !src || src->size == 0)
		return 0;

	avail = src->used - src->consumed;

	while (total < avail) {
		size_t idx = (src->tail + src->consumed + total) % src->size;
		size_t chunk = src->size - idx;
		size_t written;

		if (chunk > avail - total)
			chunk = avail - total;

		written = ringbuf_write(dst, src->buf + idx, chunk, NULL);
		total += written;
		if (written < chunk)
			break;	/* dst 空间不足，剩下的只能丢 */
	}

	/* src 的数据已经搬完（或者被判定为装不下），整体清空 */
	src->head = 0;
	src->tail = 0;
	src->rpos = 0;
	src->used = 0;
	src->consumed = 0;

	return total;
}
