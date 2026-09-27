// SPDX-License-Identifier: MIT
/*
 * labdev_main.c - linux-driver-lab 内核模块主体
 *
 * 一个基于内存虚拟设备的字符设备驱动（**没有任何真实硬件**），用来完整演示
 * Linux 字符设备驱动的主要环节：
 *
 *   misc_register() 注册设备      -> file_operations: open/release/read/write/
 *   mutex + wait_queue 并发控制       llseek/unlocked_ioctl/poll
 *   环形缓冲区                    -> sysfs 属性 + 模块参数
 *   kthread 周期性心跳            -> copy_*_user 与错误路径回滚
 *
 * 数据流：用户 write() -> 存进环形缓冲区 -> 用户 read() 取出。
 * 缓冲区满时默认覆盖最老的未读数据，并把丢弃字节数计入统计。
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/string.h>

#include "lab_ioctl.h"
#include "ringbuf.h"

/*
 * sysfs_emit() 从 5.10 开始提供，老内核上用 scnprintf() 代替。
 * 两者都是“最多写 PAGE_SIZE、返回实际长度”，语义等价。
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0)
#define sysfs_emit(buf, fmt, ...)	scnprintf((buf), PAGE_SIZE, fmt, ##__VA_ARGS__)
#endif

#define LAB_DEFAULT_BUF		4096U
#define LAB_DEFAULT_HB_MS	1000U
#define LAB_MAX_HB_MS		60000U

/* 一次 write() 最多拷贝到内核栈外的字节数，上限就是缓冲区最大容量 */
#define LAB_MAX_XFER		LAB_MAX_BUF

struct lab_dev {
	/* 必须是第一个成员：open() 里用 container_of() 从 miscdevice 反推 */
	struct miscdevice mdev;

	struct mutex lock;		/* 保护下面所有可变状态 */
	struct wait_queue_head wq;	/* 阻塞读的等待队列 */
	struct ringbuf rb;
	unsigned char *mem;		/* 环形缓冲区实际内存 */

	/* 统计（全部在 lock 保护下更新） */
	u64 bytes_written;
	u64 bytes_read;
	u64 bytes_dropped;
	u64 wait_count;
	u64 poll_count;
	u64 write_calls;
	u64 read_calls;
	u64 ioctl_calls;
	u64 open_count;
	u64 heartbeat;

	unsigned int read_chunk;	/* 单次 read 上限，0 = 不限 */
	unsigned int flags;		/* LAB_FLAG_* */
	bool debug;			/* sysfs 的 debug 属性：运行期逐次操作日志 */

	struct task_struct *kthread;
};

static struct lab_dev *labdev_dev;

/* ------------------------------------------------------------------ */
/* 模块参数                                                             */
/* ------------------------------------------------------------------ */

static unsigned int buf_size = LAB_DEFAULT_BUF;
module_param(buf_size, uint, 0444);
MODULE_PARM_DESC(buf_size, "环形缓冲区容量(字节), 范围 1024..1048576, 默认 4096; 只能在加载时指定");

static bool verbose = true;
module_param(verbose, bool, 0444);
MODULE_PARM_DESC(verbose, "是否打印模块加载/卸载日志, 默认 true; 只能在加载时指定");

/*
 * heartbeat_ms 的权限是 0644：内核线程每一轮都直接读这个全局变量，
 * 所以运行期改 /sys/module/labdev/parameters/heartbeat_ms 会立即生效。
 */
static unsigned int heartbeat_ms = LAB_DEFAULT_HB_MS;
module_param(heartbeat_ms, uint, 0644);
MODULE_PARM_DESC(heartbeat_ms, "内核线程心跳周期(毫秒), 范围 0..60000, 0 表示不启动内核线程, 默认 1000");

/* ------------------------------------------------------------------ */
/* 小工具                                                               */
/* ------------------------------------------------------------------ */

/* misc_register() 会把 miscdevice 指针存进 this_device 的 drvdata */
static struct lab_dev *labdev_from_device(struct device *d)
{
	return container_of(dev_get_drvdata(d), struct lab_dev, mdev);
}

#define labdev_dbg(dev, fmt, ...)					\
	do {								\
		if ((dev)->debug && (dev)->mdev.this_device)		\
			dev_info((dev)->mdev.this_device, fmt, ##__VA_ARGS__); \
	} while (0)

/*
 * 在持有 dev->lock 的前提下换一块新缓冲区。
 * 旧缓冲区里“还没被读走”的数据会被搬过去；装不下的部分计入 bytes_dropped。
 * 全程持有 mutex，所以读者/写者不可能看到半新半旧的状态。
 */
static int labdev_resize_locked(struct lab_dev *dev, unsigned int new_size)
{
	struct ringbuf newrb;
	unsigned char *mem;
	size_t before, moved;

	if (new_size < LAB_MIN_BUF || new_size > LAB_MAX_BUF)
		return -EINVAL;
	if (new_size == (unsigned int)dev->rb.size)
		return 0;

	/*
	 * kvmalloc(): 先试 kmalloc（物理连续、访问快），失败再退回 vmalloc。
	 * 缓冲区最大 1 MiB，直接 kmalloc 需要高阶连续页，碎片化时容易失败。
	 */
	mem = kvmalloc(new_size, GFP_KERNEL);
	if (!mem)
		return -ENOMEM;

	/* 新缓冲区先搭好再切换：任何一步失败都不会破坏旧状态 */
	ringbuf_setup(&newrb, mem, (size_t)new_size);
	before = ringbuf_avail(&dev->rb);
	moved = ringbuf_transfer(&newrb, &dev->rb);

	kvfree(dev->mem);
	dev->mem = mem;
	dev->rb = newrb;
	dev->bytes_dropped += before - moved;

	/* 空间/数据都变了，叫醒可能正在等待的读者 */
	wake_up_interruptible(&dev->wq);

	return 0;
}

/* ------------------------------------------------------------------ */
/* file_operations                                                      */
/* ------------------------------------------------------------------ */

static int labdev_open(struct inode *inode, struct file *filp)
{
	struct lab_dev *dev = container_of(filp->private_data, struct lab_dev, mdev);

	/*
	 * 打开期间把模块引用计数 +1：这样 rmmod 会直接报 EBUSY，
	 * 而不是在还有 fd 指向模块代码时把代码卸载掉。
	 */
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	/*
	 * 刻意不调用 nonseekable_open()/stream_open()：它们会清掉
	 * FMODE_LSEEK，VFS 就不会再调用我们实现的 llseek 了。
	 */
	filp->private_data = dev;

	mutex_lock(&dev->lock);
	dev->open_count++;
	mutex_unlock(&dev->lock);

	labdev_dbg(dev, "open: 累计打开 %llu 次\n", dev->open_count);
	return 0;
}

static int labdev_release(struct inode *inode, struct file *filp)
{
	struct lab_dev *dev = filp->private_data;

	labdev_dbg(dev, "release\n");
	module_put(THIS_MODULE);
	return 0;
}

static ssize_t labdev_read(struct file *filp, char __user *ubuf, size_t count,
			   loff_t *ppos)
{
	struct lab_dev *dev = filp->private_data;
	unsigned char *kbuf;
	size_t avail, want, got;
	int ret;

	if (count == 0)
		return 0;

	/* 先做一次用户地址范围检查，非法指针在拷贝前就被挡掉 */
	if (!access_ok(ubuf, count))
		return -EFAULT;

	ret = mutex_lock_interruptible(&dev->lock);
	if (ret)
		return ret;

	for (;;) {
		avail = ringbuf_avail(&dev->rb);
		if (avail > 0)
			break;

		if (filp->f_flags & O_NONBLOCK) {
			mutex_unlock(&dev->lock);
			return -EAGAIN;
		}

		/*
		 * 阻塞读：必须先放开 mutex 再睡，否则写者会被我们挡在门外。
		 * wait_event_interruptible() 的语义是“先挂进等待队列、再复查条件”，
		 * 所以不存在“条件已经成立却错过唤醒”的竞态。
		 * 这里读 dev->rb 没有加锁，只是一个无害的提示性判断——
		 * 醒来后会重新加锁并回到循环开头重新检查。
		 */
		dev->wait_count++;
		mutex_unlock(&dev->lock);

		ret = wait_event_interruptible(dev->wq,
					       dev->rb.used != dev->rb.consumed);
		if (ret)
			return -ERESTARTSYS;

		ret = mutex_lock_interruptible(&dev->lock);
		if (ret)
			return ret;
	}

	want = (count < avail) ? count : avail;
	if (dev->read_chunk && want > dev->read_chunk)
		want = dev->read_chunk;

	/*
	 * 用 kvmalloc 而不是 kmalloc：want 最大可以到缓冲区容量（上限 1 MiB），
	 * kmalloc 需要高阶连续页，碎片化时容易失败；kvmalloc 会退回 vmalloc。
	 * 释放必须用配对的 kvfree()。
	 */
	kbuf = kvmalloc(want, GFP_KERNEL);
	if (!kbuf) {
		mutex_unlock(&dev->lock);
		return -ENOMEM;
	}

	got = ringbuf_read(&dev->rb, kbuf, want);

	/*
	 * copy_to_user() 可能因为缺页而睡眠，而 mutex 允许睡眠，
	 * 所以这里可以持锁拷贝（spinlock 就不行）。持锁拷贝的额外好处是
	 * 拷贝失败时可以安全回滚读游标，不会在用户态“凭空丢数据”。
	 */
	if (copy_to_user(ubuf, kbuf, got)) {
		ringbuf_seek(&dev->rb, -(long)got, RB_SEEK_CUR);
		kvfree(kbuf);
		mutex_unlock(&dev->lock);
		return -EFAULT;
	}

	kvfree(kbuf);
	dev->bytes_read += got;
	dev->read_calls++;
	mutex_unlock(&dev->lock);

	labdev_dbg(dev, "read: 返回 %zu 字节\n", got);
	return (ssize_t)got;
}

static ssize_t labdev_write(struct file *filp, const char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	struct lab_dev *dev = filp->private_data;
	unsigned char *kbuf;
	size_t want, written, dropped = 0;
	int ret;

	if (count == 0)
		return 0;

	if (!access_ok(ubuf, count))
		return -EFAULT;

	/*
	 * 单次 write 最多接受“缓冲区容量”那么多字节，这里先按上限分配，
	 * 免得用户传个 SIZE_MAX 之类的值去申请一大块内存。
	 * 上限 1 MiB 属于高阶分配，所以用 kvmalloc（配 kvfree 释放）。
	 */
	want = (count < LAB_MAX_XFER) ? count : (size_t)LAB_MAX_XFER;

	/* 锁外分配并拷贝：copy_from_user() 会睡眠，不该放在临界区里 */
	kbuf = kvmalloc(want, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	if (copy_from_user(kbuf, ubuf, want)) {
		kvfree(kbuf);
		return -EFAULT;
	}

	ret = mutex_lock_interruptible(&dev->lock);
	if (ret) {
		kvfree(kbuf);
		return ret;
	}

	/* 严格模式：装不下就报错，绝不悄悄丢掉用户数据 */
	if ((dev->flags & LAB_FLAG_NO_DROP) &&
	    ringbuf_space(&dev->rb) < want) {
		mutex_unlock(&dev->lock);
		kvfree(kbuf);
		return -ENOSPC;
	}

	written = ringbuf_write(&dev->rb, kbuf, want, &dropped);
	dev->bytes_written += written;
	dev->bytes_dropped += dropped;
	dev->write_calls++;
	mutex_unlock(&dev->lock);

	kvfree(kbuf);

	if (written)
		wake_up_interruptible(&dev->wq);

	labdev_dbg(dev, "write: 请求 %zu, 接受 %zu, 丢弃 %zu\n",
		   count, written, dropped);

	/*
	 * 缓冲区满时可能短写（返回值 < count）。写操作本身永不阻塞，
	 * 因此这里不需要处理 O_NONBLOCK。
	 */
	return (ssize_t)written;
}

/*
 * llseek 的语义是“缓冲区相对偏移”，不是文件偏移：
 *   SEEK_SET: 相对缓冲区里最老的那个字节
 *   SEEK_CUR: 相对当前读游标
 *   SEEK_END: 相对缓冲区末尾（已缓冲数据之后）
 * 读出过的数据在被新数据覆盖之前仍然留在缓冲区里，所以可以回退重读；
 * 一旦被覆盖（计入 bytes_dropped），回退就会以 -EINVAL 失败。
 */
static loff_t labdev_llseek(struct file *filp, loff_t off, int whence)
{
	struct lab_dev *dev = filp->private_data;
	long pos = -1;
	int rb_whence;

	if (off < LONG_MIN || off > LONG_MAX)
		return -EINVAL;

	switch (whence) {
	case SEEK_SET:
		rb_whence = RB_SEEK_SET;
		break;
	case SEEK_CUR:
		rb_whence = RB_SEEK_CUR;
		break;
	case SEEK_END:
		rb_whence = RB_SEEK_END;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&dev->lock);
	pos = ringbuf_seek(&dev->rb, (long)off, rb_whence);
	mutex_unlock(&dev->lock);

	if (pos < 0)
		return -EINVAL;

	return (loff_t)pos;
}

/*
 * poll()：设备可读的条件是“还有未读数据”。
 * 写永远可完成（满了就覆盖最老数据），所以始终上报 EPOLLOUT。
 * 必须先 poll_wait() 把当前进程挂进等待队列，再检查状态，
 * 否则会有“检查完状态、还没来得及挂队列，数据就到了”的丢唤醒问题。
 */
static __poll_t labdev_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct lab_dev *dev = filp->private_data;
	__poll_t mask = 0;

	poll_wait(filp, &dev->wq, wait);

	mutex_lock(&dev->lock);
	dev->poll_count++;
	if (ringbuf_avail(&dev->rb) > 0)
		mask |= EPOLLIN | EPOLLRDNORM;
	mask |= EPOLLOUT | EPOLLWRNORM;
	mutex_unlock(&dev->lock);

	return mask;
}

/* ------------------------------------------------------------------ */
/* ioctl                                                                */
/* ------------------------------------------------------------------ */

static long labdev_ioc_get_config(struct lab_dev *dev, void __user *uarg)
{
	struct lab_config cfg;

	mutex_lock(&dev->lock);
	cfg.buffer_size = (__u32)ringbuf_size(&dev->rb);
	cfg.read_chunk = dev->read_chunk;
	cfg.flags = dev->flags;
	cfg.reserved = 0;
	mutex_unlock(&dev->lock);

	if (copy_to_user(uarg, &cfg, sizeof(cfg)))
		return -EFAULT;

	return 0;
}

static long labdev_ioc_set_config(struct lab_dev *dev, void __user *uarg)
{
	struct lab_config cfg, applied;
	int ret;

	if (copy_from_user(&cfg, uarg, sizeof(cfg)))
		return -EFAULT;

	/* 参数校验必须在动手改状态之前做完 */
	if (cfg.reserved != 0)
		return -EINVAL;
	if (cfg.flags & ~(__u32)LAB_FLAG_ALL)
		return -EINVAL;
	if (cfg.buffer_size &&
	    (cfg.buffer_size < LAB_MIN_BUF || cfg.buffer_size > LAB_MAX_BUF))
		return -EINVAL;

	ret = mutex_lock_interruptible(&dev->lock);
	if (ret)
		return ret;

	if (cfg.buffer_size) {
		ret = labdev_resize_locked(dev, cfg.buffer_size);
		if (ret) {
			mutex_unlock(&dev->lock);
			return ret;
		}
	}

	dev->read_chunk = cfg.read_chunk;
	dev->flags = cfg.flags;

	applied.buffer_size = (__u32)ringbuf_size(&dev->rb);
	applied.read_chunk = dev->read_chunk;
	applied.flags = dev->flags;
	applied.reserved = 0;
	mutex_unlock(&dev->lock);

	/*
	 * SET_CONFIG 用 _IOWR 定义：回填“实际生效”的配置，
	 * 这样用户态能立刻看出哪些值被接受了（例如 buffer_size 传 0）。
	 */
	if (copy_to_user(uarg, &applied, sizeof(applied)))
		return -EFAULT;

	return 0;
}

static long labdev_ioc_reset(struct lab_dev *dev)
{
	mutex_lock(&dev->lock);
	ringbuf_reset(&dev->rb);
	dev->bytes_written = 0;
	dev->bytes_read = 0;
	dev->bytes_dropped = 0;
	dev->wait_count = 0;
	dev->poll_count = 0;
	dev->write_calls = 0;
	dev->read_calls = 0;
	dev->ioctl_calls = 0;
	dev->heartbeat = 0;
	mutex_unlock(&dev->lock);

	/* 缓冲区被清空，正在等待的读者要重新判断条件 */
	wake_up_interruptible(&dev->wq);

	return 0;
}

static long labdev_ioc_get_stats(struct lab_dev *dev, void __user *uarg)
{
	struct lab_stats st;

	mutex_lock(&dev->lock);
	st.bytes_written = dev->bytes_written;
	st.bytes_read = dev->bytes_read;
	st.bytes_dropped = dev->bytes_dropped;
	st.wait_count = dev->wait_count;
	st.poll_count = dev->poll_count;
	st.write_calls = dev->write_calls;
	st.read_calls = dev->read_calls;
	st.heartbeat = dev->heartbeat;
	mutex_unlock(&dev->lock);

	if (copy_to_user(uarg, &st, sizeof(st)))
		return -EFAULT;

	return 0;
}

static long labdev_unlocked_ioctl(struct file *filp, unsigned int cmd,
				  unsigned long arg)
{
	struct lab_dev *dev = filp->private_data;
	void __user *uarg = (void __user *)arg;
	long ret;

	/*
	 * 命令编码校验（照 kernel Documentation/driver-api/ioctl.rst 的推荐写法）：
	 * magic、序号、方向、参数大小都要对得上，否则说明用户态的
	 * lab_ioctl.h 和内核模块不是同一个版本，必须直接拒绝。
	 */
	if (_IOC_TYPE(cmd) != LAB_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) > LAB_IOC_MAXNR)
		return -ENOTTY;

	if ((_IOC_DIR(cmd) & _IOC_READ) && !access_ok(uarg, _IOC_SIZE(cmd)))
		return -EFAULT;
	if ((_IOC_DIR(cmd) & _IOC_WRITE) && !access_ok(uarg, _IOC_SIZE(cmd)))
		return -EFAULT;

	mutex_lock(&dev->lock);
	dev->ioctl_calls++;
	mutex_unlock(&dev->lock);

	switch (cmd) {
	case LAB_IOC_GET_CONFIG:
		ret = labdev_ioc_get_config(dev, uarg);
		break;
	case LAB_IOC_SET_CONFIG:
		ret = labdev_ioc_set_config(dev, uarg);
		break;
	case LAB_IOC_RESET:
		ret = labdev_ioc_reset(dev);
		break;
	case LAB_IOC_GET_STATS:
		ret = labdev_ioc_get_stats(dev, uarg);
		break;
	default:
		/* magic 对但序号未知：只有 header 不一致才会走到这里 */
		ret = -ENOTTY;
		break;
	}

	labdev_dbg(dev, "ioctl: cmd=0x%08x -> %ld\n", cmd, ret);
	return ret;
}

static const struct file_operations labdev_fops = {
	.owner		= THIS_MODULE,
	.open		= labdev_open,
	.release	= labdev_release,
	.read		= labdev_read,
	.write		= labdev_write,
	.llseek		= labdev_llseek,
	.unlocked_ioctl	= labdev_unlocked_ioctl,
	.poll		= labdev_poll,
};

/* ------------------------------------------------------------------ */
/* sysfs 属性                                                           */
/* ------------------------------------------------------------------ */

static ssize_t buffer_size_show(struct device *d, struct device_attribute *attr,
				char *buf)
{
	struct lab_dev *dev = labdev_from_device(d);
	unsigned int size;

	mutex_lock(&dev->lock);
	size = (unsigned int)ringbuf_size(&dev->rb);
	mutex_unlock(&dev->lock);

	return sysfs_emit(buf, "%u\n", size);
}
static DEVICE_ATTR_RO(buffer_size);

static ssize_t debug_show(struct device *d, struct device_attribute *attr,
			  char *buf)
{
	struct lab_dev *dev = labdev_from_device(d);
	bool on;

	mutex_lock(&dev->lock);
	on = dev->debug;
	mutex_unlock(&dev->lock);

	return sysfs_emit(buf, "%u\n", on ? 1 : 0);
}

static ssize_t debug_store(struct device *d, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct lab_dev *dev = labdev_from_device(d);
	unsigned int val;
	int ret;

	/* kstrtouint 而不是 kstrtobool：只接受 0/1，写别的值直接报 -EINVAL */
	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (val > 1)
		return -EINVAL;

	mutex_lock(&dev->lock);
	dev->debug = !!val;
	mutex_unlock(&dev->lock);

	return count;
}
static DEVICE_ATTR_RW(debug);

/*
 * 多行 key=value 形式的统计快照，方便脚本解析。
 * （严格来说 sysfs 推荐“一个文件一个值”，但只读的统计聚合在真实驱动里很常见。）
 */
static ssize_t stats_show(struct device *d, struct device_attribute *attr,
			  char *buf)
{
	struct lab_dev *dev = labdev_from_device(d);
	struct lab_stats st;

	mutex_lock(&dev->lock);
	st.bytes_written = dev->bytes_written;
	st.bytes_read = dev->bytes_read;
	st.bytes_dropped = dev->bytes_dropped;
	st.wait_count = dev->wait_count;
	st.poll_count = dev->poll_count;
	st.write_calls = dev->write_calls;
	st.read_calls = dev->read_calls;
	st.heartbeat = dev->heartbeat;
	mutex_unlock(&dev->lock);

	return sysfs_emit(buf,
			  "bytes_written=%llu\n"
			  "bytes_read=%llu\n"
			  "bytes_dropped=%llu\n"
			  "wait_count=%llu\n"
			  "poll_count=%llu\n"
			  "write_calls=%llu\n"
			  "read_calls=%llu\n"
			  "heartbeat=%llu\n",
			  st.bytes_written, st.bytes_read, st.bytes_dropped,
			  st.wait_count, st.poll_count, st.write_calls,
			  st.read_calls, st.heartbeat);
}
static DEVICE_ATTR_RO(stats);

static struct attribute *labdev_attrs[] = {
	&dev_attr_buffer_size.attr,
	&dev_attr_debug.attr,
	&dev_attr_stats.attr,
	NULL,
};
ATTRIBUTE_GROUPS(labdev);

/* ------------------------------------------------------------------ */
/* 内核线程                                                             */
/* ------------------------------------------------------------------ */

/*
 * 周期性心跳线程：每隔 heartbeat_ms 把 dev->heartbeat 加一。
 * 目的是演示“内核线程的创建、周期睡眠、以及卸载时的安全停止”。
 *
 * 安全性要点：
 *  1. 睡眠用 msleep_interruptible()，它可被 kthread_stop() 内部的
 *     wake_up_process() 提前唤醒，因此 rmmod 不必等满一个周期。
 *  2. 循环条件始终检查 kthread_should_stop()，保证能退出。
 *  3. 线程只做“短暂持锁 + 自增”，不会长时间占用 mutex。
 */
static int labdev_kthread_fn(void *data)
{
	struct lab_dev *dev = data;

	while (!kthread_should_stop()) {
		mutex_lock(&dev->lock);
		dev->heartbeat++;
		mutex_unlock(&dev->lock);

		/*
		 * 直接读全局 heartbeat_ms（而不是缓存的副本），
		 * 这样运行期改模块参数能立刻生效。
		 * 中断/被 kthread_stop 唤醒时提前返回，下一轮循环就会看到
		 * kthread_should_stop() == true 并退出。
		 */
		msleep_interruptible(heartbeat_ms ? heartbeat_ms : 1);
	}

	return 0;
}

static void labdev_stop_kthread(struct lab_dev *dev)
{
	if (dev->kthread) {
		/* 会阻塞到线程真正返回为止 */
		kthread_stop(dev->kthread);
		dev->kthread = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* 模块初始化/卸载                                                      */
/* ------------------------------------------------------------------ */

static int __init labdev_init(void)
{
	struct lab_dev *dev;
	int ret;

	if (buf_size < LAB_MIN_BUF || buf_size > LAB_MAX_BUF) {
		pr_err("buf_size=%u 超出范围 [%u, %u]\n",
		       buf_size, LAB_MIN_BUF, LAB_MAX_BUF);
		return -EINVAL;
	}
	if (heartbeat_ms > LAB_MAX_HB_MS) {
		pr_err("heartbeat_ms=%u 超出范围 [0, %u]\n",
		       heartbeat_ms, LAB_MAX_HB_MS);
		return -EINVAL;
	}

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->mem = kvmalloc(buf_size, GFP_KERNEL);
	if (!dev->mem) {
		kfree(dev);
		return -ENOMEM;
	}

	mutex_init(&dev->lock);
	init_waitqueue_head(&dev->wq);
	ringbuf_setup(&dev->rb, dev->mem, (size_t)buf_size);

	dev->mdev.minor = MISC_DYNAMIC_MINOR;
	dev->mdev.name = LAB_DEV_NAME;
	dev->mdev.fops = &labdev_fops;
	dev->mdev.groups = labdev_groups;
	/*
	 * 0600：只有 root 能打开。真实驱动同样应该收紧权限、再靠 udev 规则
	 * 给特定用户放权，而不是图省事设成 0666。
	 */
	dev->mdev.mode = 0600;

	ret = misc_register(&dev->mdev);
	if (ret) {
		pr_err("misc_register() 失败: %d\n", ret);
		goto err_free_mem;
	}

	if (heartbeat_ms) {
		dev->kthread = kthread_run(labdev_kthread_fn, dev, LAB_DEV_NAME "-hb");
		if (IS_ERR(dev->kthread)) {
			ret = PTR_ERR(dev->kthread);
			dev->kthread = NULL;
			pr_err("kthread_run() 失败: %d\n", ret);
			goto err_deregister;
		}
	}

	labdev_dev = dev;

	if (verbose)
		pr_info("已加载: /dev/%s, 缓冲区 %u 字节, 心跳 %u ms, 内核线程: %s\n",
			LAB_DEV_NAME, buf_size, heartbeat_ms,
			dev->kthread ? "已启动" : "未启动");
	return 0;

	/* 错误路径逐级回滚，顺序与申请顺序相反 */
err_deregister:
	misc_deregister(&dev->mdev);
err_free_mem:
	kvfree(dev->mem);
	kfree(dev);
	return ret;
}

static void __exit labdev_exit(void)
{
	struct lab_dev *dev = labdev_dev;

	if (!dev)
		return;

	/*
	 * 顺序非常关键：先停内核线程，再碰 dev->lock。
	 * 如果反过来（先加锁再 kthread_stop），线程可能正卡在
	 * mutex_lock(&dev->lock) 上等锁，而 kthread_stop() 又在等它退出
	 * —— 直接死锁。所以这里第一步就 kthread_stop()。
	 */
	labdev_stop_kthread(dev);

	/* 之后 /dev/labdev 与 sysfs 属性都会消失 */
	misc_deregister(&dev->mdev);

	kvfree(dev->mem);
	kfree(dev);
	labdev_dev = NULL;

	if (verbose)
		pr_info("已卸载, 无残留\n");
}

module_init(labdev_init);
module_exit(labdev_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("linux-driver-lab");
MODULE_DESCRIPTION("基于内存虚拟设备的字符设备驱动示例 (misc + ringbuf + mutex + waitqueue + kthread)");
MODULE_VERSION("1.0.0");
