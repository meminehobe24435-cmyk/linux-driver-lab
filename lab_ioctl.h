/* SPDX-License-Identifier: MIT */
/*
 * lab_ioctl.h - labdev 字符设备的 ioctl ABI 定义
 *
 * 这个头文件同时被内核模块和用户态程序包含，是二者之间唯一的契约。
 * 结构体只用 __u32/__u64 这类定宽类型，并带编译期大小断言，
 * 避免 32/64 位或对齐差异导致的 ABI 漂移。
 */
#ifndef _LAB_IOCTL_H
#define _LAB_IOCTL_H

#ifdef __KERNEL__
#  include <linux/types.h>
#  include <linux/ioctl.h>
#  include <linux/build_bug.h>
#  define LAB_STATIC_ASSERT(cond, msg)	static_assert(cond, msg)
#else
#  include <linux/types.h>	/* 由 linux-libc-dev 提供 __u32/__u64 */
#  include <sys/ioctl.h>	/* 提供 _IO/_IOR/_IOW/_IOWR 宏 */
#  if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L)
#    define LAB_STATIC_ASSERT(cond, msg)	_Static_assert(cond, msg)
#  else
#    define LAB_STATIC_ASSERT(cond, msg)	/* 老编译器上跳过 */
#  endif
#endif

/* 设备名：模块名 labdev，设备节点 /dev/labdev */
#define LAB_DEV_NAME	"labdev"

/* 缓冲区容量范围，SET_CONFIG 与 buf_size 模块参数都受这个范围约束 */
#define LAB_MIN_BUF	1024U
#define LAB_MAX_BUF	(1024U * 1024U)

/* struct lab_config.flags 的合法位 */
#define LAB_FLAG_NO_DROP	(1U << 0)	/* 满时返回 -ENOSPC 而不覆盖老数据 */
#define LAB_FLAG_ALL		(LAB_FLAG_NO_DROP)

/*
 * struct lab_config - 设备配置
 * @buffer_size: 环形缓冲区字节数，写入 0 表示“保持不变”
 * @read_chunk:  单次 read() 最多返回的字节数，0 表示不限制
 * @flags:       LAB_FLAG_* 位掩码，未知位会被内核拒绝（-EINVAL）
 * @reserved:    必须为 0，留给后续扩展
 */
struct lab_config {
	__u32 buffer_size;
	__u32 read_chunk;
	__u32 flags;
	__u32 reserved;
};

/*
 * struct lab_stats - 统计计数器，全部是自设备加载（或上次 RESET）以来的累计值
 */
struct lab_stats {
	__u64 bytes_written;	/* 用户写入并被接受的字节数 */
	__u64 bytes_read;	/* 用户读走的字节数 */
	__u64 bytes_dropped;	/* 因缓冲区满被覆盖掉的未读字节数 */
	__u64 wait_count;	/* 阻塞读进入睡眠的次数 */
	__u64 poll_count;	/* poll()/epoll 被调用的次数 */
	__u64 write_calls;	/* write() 调用次数 */
	__u64 read_calls;	/* read() 调用次数 */
	__u64 heartbeat;	/* 内核线程心跳计数 */
};

/* ABI 大小固定，改结构体必须同步改这里的数字和 README 里的表 */
LAB_STATIC_ASSERT(sizeof(struct lab_config) == 16, "lab_config ABI 大小必须为 16 字节");
LAB_STATIC_ASSERT(sizeof(struct lab_stats) == 64, "lab_stats ABI 大小必须为 64 字节");

#define LAB_IOC_MAGIC	'L'

/*
 * ioctl 命令表（README 里有对应文档）
 *   _IOR  : 内核 -> 用户（只读）
 *   _IOWR : 双向（用户 -> 内核 -> 用户）
 *   _IO   : 无参数
 * 命令编码里带 magic / 序号 / 方向 / 结构体大小，内核会逐项校验，
 * 因此“用户态头文件版本不匹配”会被直接拒绝，而不是写坏内存。
 */
#define LAB_IOC_GET_CONFIG	_IOR(LAB_IOC_MAGIC, 1, struct lab_config)
#define LAB_IOC_SET_CONFIG	_IOWR(LAB_IOC_MAGIC, 2, struct lab_config)
#define LAB_IOC_RESET		_IO(LAB_IOC_MAGIC, 3)
#define LAB_IOC_GET_STATS	_IOR(LAB_IOC_MAGIC, 4, struct lab_stats)

/* 已知的最大命令序号，用于拒绝越界序列号 */
#define LAB_IOC_MAXNR		4

#endif /* _LAB_IOCTL_H */
