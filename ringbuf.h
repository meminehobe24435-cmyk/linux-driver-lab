/* SPDX-License-Identifier: MIT */
/*
 * ringbuf.h - 与内核无关的环形缓冲区核心逻辑
 *
 * 这个头文件刻意不包含任何内核头文件：
 * 同一份 ringbuf.c 既被内核模块 labdev_main.c 使用，也被
 * tests/ringbuf_test.c 在用户态直接编译测试。缓冲区内存由调用者提供
 * （内核侧用 kvmalloc()，用户态用 malloc()），因此两侧跑的是完全相同的算法。
 *
 * 数据模型
 * --------
 *   buf       : 外部提供的内存
 *   size      : 内存字节数（容量）
 *   head      : 下一个写入位置
 *   tail      : 缓冲区中最老数据的下标
 *   used      : 缓冲区中有效字节数 = (head - tail) mod size
 *   rpos      : 读游标（绝对下标）
 *   consumed  : 读游标已经越过、但仍占着物理空间的字节数
 *
 * 未读字节数 avail = used - consumed。读游标可以后退（llseek 依赖这一点），
 * 只要那些字节还没有被新数据覆盖掉。
 *
 * 本文件不使用任何原子操作或锁：并发保护由调用方（内核模块的 mutex）负责。
 */
#ifndef _LAB_RINGBUF_H
#define _LAB_RINGBUF_H

#include <stddef.h>

/* 自己定义 whence，避免同时依赖 <unistd.h> 和 <linux/fs.h> */
#define RB_SEEK_SET	0
#define RB_SEEK_CUR	1
#define RB_SEEK_END	2

struct ringbuf {
	unsigned char *buf;
	size_t size;
	size_t head;
	size_t tail;
	size_t rpos;
	size_t used;
	size_t consumed;
};

/*
 * ringbuf_setup() - 用一块现成的内存初始化环形缓冲区
 * @rb:   待初始化的结构体
 * @mem:  内存首地址
 * @size: 内存字节数；小于 2 时视为非法，缓冲区被置为不可用（所有操作返回 0）
 *
 * 不申请内存、不释放内存。
 */
void ringbuf_setup(struct ringbuf *rb, unsigned char *mem, size_t size);

/* ringbuf_reset() - 丢弃全部数据，回到初始状态（不动内存） */
void ringbuf_reset(struct ringbuf *rb);

/* 容量（字节） */
size_t ringbuf_size(const struct ringbuf *rb);

/* 缓冲区里还留着的有效字节数（含已被读过、但尚未回收的部分） */
size_t ringbuf_used(const struct ringbuf *rb);

/* 还能读出的字节数 */
size_t ringbuf_avail(const struct ringbuf *rb);

/* 在不丢弃任何未读数据的前提下，还能写入多少字节 */
size_t ringbuf_space(const struct ringbuf *rb);

/*
 * ringbuf_write() - 写入数据
 * @rb:      缓冲区
 * @src:     源数据
 * @len:     请求写入长度
 * @dropped: 输出参数，返回本次被覆盖掉的“未读”字节数，可为 NULL
 *
 * 返回实际接受的字节数。语义要点：
 *  - 空间不足时先回收读游标已越过的字节（不丢数据）；
 *  - 仍然不足则覆盖最老的未读字节，并计入 @dropped；
 *  - 若 @len 大于容量，只接受前 size 字节（驱动层已把单次写限制在 buffer_size 内）。
 */
size_t ringbuf_write(struct ringbuf *rb, const unsigned char *src, size_t len,
		     size_t *dropped);

/*
 * ringbuf_read() - 读出数据并推进读游标
 * 返回实际读出的字节数；缓冲区无数据时返回 0。
 *
 * 关键语义：读走的数据**不会**被立即丢弃，而是留在缓冲区里直到被新写入
 * 覆盖（那些空间由 ringbuf_write() 回收）。因此 llseek 可以回退重读，
 * 而“回退能力什么时候消失”只有一个答案：数据被覆盖的时候。
 */
size_t ringbuf_read(struct ringbuf *rb, unsigned char *dst, size_t len);

/*
 * ringbuf_seek() - 移动读游标
 * @offset: 相对偏移，可为负
 * @whence: RB_SEEK_SET / RB_SEEK_CUR / RB_SEEK_END
 * 返回新的读游标位置（相对最老字节的偏移）；越界返回 -1。
 */
long ringbuf_seek(struct ringbuf *rb, long offset, int whence);

/*
 * ringbuf_transfer() - 把 src 中尚未读走的数据搬进 dst，并清空 src
 * @dst 必须为空且容量已就绪。返回搬运成功的字节数；
 * dst 装不下的部分会被丢弃（调用方负责记账）。
 */
size_t ringbuf_transfer(struct ringbuf *dst, struct ringbuf *src);

#endif /* _LAB_RINGBUF_H */
