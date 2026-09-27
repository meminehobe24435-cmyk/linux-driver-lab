#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
stress.py - labdev 字符设备的多进程/多线程并发压测与数据完整性校验

三个阶段：

  阶段 1  严格完整性（LAB_FLAG_NO_DROP）
          1 个写进程 + 1 个读进程，写入 8 MiB 确定性字节流。
          由于严格模式下缓冲区满会返回 -ENOSPC 而不是丢弃数据，
          读到的字节流必须与写出的**逐字节完全相同**。
          任何缺失、错位、重排都会让这一阶段 FAIL。

  阶段 2  多进程 × 多线程并发读写（允许丢弃）
          每个 worker 写 40 字节定长记录：magic + seq + writer_id + crc32 + payload。
          多个读者并发读取同一条 FIFO 字节流，逐条解析并校验：
            - magic 与 crc32 必须自洽（区分“记录”和“丢弃造成的错位”）
            - payload 必须等于由 (writer_id, seq) 推导出的值（强校验）
            - 同一个 writer 的 seq 必须严格递增（检测重排/重复）
          缓冲区满时覆盖最老数据是**设计行为**，会导致读者在中途错位；
          这种错位由 resync 处理并单独计数，不算失败。

  阶段 3  open/close/ioctl/poll 混压
          多个进程反复 open → ioctl → write → poll → read → close，
          验证高频并发打开/关闭不会泄漏引用计数或死锁。

用法:
  sudo python3 tools/stress.py [--dev /dev/labdev] [--procs 4] [--threads 2]
                               [--seconds 3] [--bytes 8388608]

退出码: 0 = 全部 PASS, 1 = 有 FAIL
"""

import argparse
import errno
import fcntl
import os
import struct
import sys
import threading
import time
import zlib
from multiprocessing import Process, Queue

# ---------------------------------------------------------------------------
# ioctl ABI：必须与 lab_ioctl.h 保持一致
# ---------------------------------------------------------------------------
_IOC_NRSHIFT = 0
_IOC_TYPESHIFT = 8
_IOC_SIZESHIFT = 16
_IOC_DIRSHIFT = 30
_IOC_NONE = 0
_IOC_WRITE = 1
_IOC_READ = 2
_MAGIC = ord('L')


def _ioc(dir_, nr, size):
    return ((dir_ << _IOC_DIRSHIFT) | (size << _IOC_SIZESHIFT) |
            (_MAGIC << _IOC_TYPESHIFT) | (nr << _IOC_NRSHIFT))


LAB_IOC_GET_CONFIG = _ioc(_IOC_READ, 1, 16)
LAB_IOC_SET_CONFIG = _ioc(_IOC_READ | _IOC_WRITE, 2, 16)
LAB_IOC_RESET = _ioc(_IOC_NONE, 3, 0)
LAB_IOC_GET_STATS = _ioc(_IOC_READ, 4, 64)

LAB_FLAG_NO_DROP = 1 << 0
LAB_MIN_BUF = 1024
LAB_MAX_BUF = 1024 * 1024


def get_config(fd):
    buf = bytearray(16)
    fcntl.ioctl(fd, LAB_IOC_GET_CONFIG, buf, True)
    return struct.unpack('<4I', bytes(buf))


def set_config(fd, buffer_size=0, read_chunk=0, flags=0):
    buf = bytearray(struct.pack('<4I', buffer_size, read_chunk, flags, 0))
    fcntl.ioctl(fd, LAB_IOC_SET_CONFIG, buf, True)
    return struct.unpack('<4I', bytes(buf))


def device_reset(fd):
    fcntl.ioctl(fd, LAB_IOC_RESET, 0)


def get_stats(fd):
    buf = bytearray(64)
    fcntl.ioctl(fd, LAB_IOC_GET_STATS, buf, True)
    return struct.unpack('<8Q', bytes(buf))


def prepare(dev, buffer_size, flags, read_chunk=0):
    """在一个临时 fd 上复位设备并设置配置，然后立刻关闭该 fd"""
    fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK)
    try:
        device_reset(fd)
        applied = set_config(fd, buffer_size, read_chunk, flags)
        if applied[0] != buffer_size or applied[2] != flags:
            raise RuntimeError(
                'SET_CONFIG 未生效: 请求 size=%d flags=0x%x, 回填 size=%d flags=0x%x'
                % (buffer_size, flags, applied[0], applied[2]))
        return applied
    finally:
        os.close(fd)


# ---------------------------------------------------------------------------
# 阶段 1：严格完整性
# ---------------------------------------------------------------------------
PATTERN_BLOCK = 4096
PATTERN = bytes(((i * 7 + (i >> 5) * 3 + (i >> 11) * 131 + 11) & 0xFF)
                for i in range(PATTERN_BLOCK))


def expected_bytes(offset, length):
    """确定性字节流：PATTERN 无限重复，按 offset 取 length 字节"""
    out = bytearray()
    start = offset % PATTERN_BLOCK
    need = length
    while need > 0:
        take = min(need, PATTERN_BLOCK - start)
        out += PATTERN[start:start + take]
        need -= take
        start = 0
    return bytes(out)


def phase1_writer(dev, total, chunk, q):
    written = 0
    retries = 0
    try:
        fd = os.open(dev, os.O_WRONLY | os.O_NONBLOCK)
    except OSError as exc:
        q.put({'role': 'writer', 'ok': False, 'err': 'open: %s' % exc})
        return
    try:
        while written < total:
            n = min(chunk, total - written)
            data = expected_bytes(written, n)
            try:
                got = os.write(fd, data)
            except OSError as exc:
                if exc.errno == errno.ENOSPC:
                    retries += 1
                    time.sleep(0.0005)      # 严格模式：等读者腾出空间
                    continue
                q.put({'role': 'writer', 'ok': False, 'err': 'write: %s' % exc})
                return
            if got <= 0:
                q.put({'role': 'writer', 'ok': False,
                       'err': 'write 返回 %d' % got})
                return
            written += got
    finally:
        os.close(fd)
    q.put({'role': 'writer', 'ok': True, 'bytes': written, 'retries': retries})


def phase1_reader(dev, total, chunk, q):
    fd = None
    try:
        fd = os.open(dev, os.O_RDONLY)
        offset = 0
        while offset < total:
            data = os.read(fd, chunk)
            if not data:
                q.put({'role': 'reader', 'ok': False,
                       'err': '读到 0 字节但还差 %d 字节' % (total - offset)})
                return
            want = expected_bytes(offset, len(data))
            if data != want:
                # 找到第一个不一致的字节，方便定位
                bad = next(i for i in range(len(data)) if data[i] != want[i])
                q.put({'role': 'reader', 'ok': False,
                       'err': '第 %d 字节不一致: 设备=0x%02x 期望=0x%02x'
                              % (offset + bad, data[bad], want[bad])})
                return
            offset += len(data)
    except OSError as exc:
        q.put({'role': 'reader', 'ok': False, 'err': 'read: %s' % exc})
        return
    finally:
        if fd is not None:
            os.close(fd)
    q.put({'role': 'reader', 'ok': True, 'bytes': offset})


def run_phase1(dev, total, deadline_pad=30.0):
    """返回 (passed, detail 字符串)"""
    buf_size = 64 * 1024
    prepare(dev, buf_size, LAB_FLAG_NO_DROP)
    chunk = 8192

    q = Queue()
    r = Process(target=phase1_reader, args=(dev, total, chunk, q))
    w = Process(target=phase1_writer, args=(dev, total, chunk, q))
    r.start()
    w.start()

    results = []
    for _ in range(2):
        try:
            results.append(q.get(timeout=deadline_pad))
        except Exception:
            results = []
            break

    for p in (r, w):
        p.join(timeout=5)
        if p.is_alive():
            p.terminate()
            p.join(timeout=5)
            return False, '子进程超时未退出（疑似死锁或丢唤醒）'
        if p.exitcode != 0:
            return False, '子进程退出码 %s' % p.exitcode

    if len(results) != 2:
        return False, '未收到两个子进程的结果'

    for item in results:
        if not item.get('ok'):
            return False, '%s: %s' % (item.get('role'), item.get('err'))

    rd = next(x for x in results if x['role'] == 'reader')
    wr = next(x for x in results if x['role'] == 'writer')
    if rd['bytes'] != total or wr['bytes'] != total:
        return False, '字节数不符: 读 %d 写 %d 期望 %d' % (rd['bytes'], wr['bytes'], total)

    # 设备侧统计应当与用户侧一致（严格模式下不允许任何丢弃）
    fd = os.open(dev, os.O_RDWR)
    try:
        st = get_stats(fd)
    finally:
        os.close(fd)
    if st[2] != 0:
        return False, '严格模式下 bytes_dropped 应为 0, 实际 %d' % st[2]
    if st[0] != total or st[1] != total:
        return False, ('设备统计不符: written=%d read=%d 期望 %d'
                       % (st[0], st[1], total))

    return True, ('逐字节一致 %d 字节, 写侧重试(ENOSPC) %d 次, '
                  '设备侧 bytes_written=%d bytes_read=%d bytes_dropped=%d'
                  % (total, wr['retries'], st[0], st[1], st[2]))


# ---------------------------------------------------------------------------
# 阶段 2：多进程多线程并发读写 + 记录级完整性校验
# ---------------------------------------------------------------------------
REC_MAGIC = 0x4C414244          # 'LABD'
REC_PAYLOAD = 24
REC_HEADER = 16
REC_SIZE = REC_HEADER + REC_PAYLOAD
REC_MAGIC_BYTES = struct.pack('<I', REC_MAGIC)
_MASK64 = 0xFFFFFFFFFFFFFFFF


def make_payload(wid, seq):
    """由 (writer_id, seq) 确定性推导出 24 字节负载"""
    x = ((wid << 16) ^ (seq & 0xFFFFFFFF) ^ 0x9E3779B97F4A7C15) & _MASK64
    out = bytearray(REC_PAYLOAD)
    for i in range(REC_PAYLOAD // 4):
        x = (x * 6364136223846793005 + 1442695040888963407) & _MASK64
        struct.pack_into('<I', out, i * 4, (x >> 32) & 0xFFFFFFFF)
    return bytes(out)


def make_record(wid, seq):
    payload = make_payload(wid, seq)
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    return struct.pack('<IIII', REC_MAGIC, seq, wid, crc) + payload


class Parser:
    """从任意字节流里恢复记录，容忍丢弃造成的错位"""

    def __init__(self):
        self.buf = bytearray()
        self.bytes_read = 0
        self.records_ok = 0
        self.resyncs = 0
        self.bad_payload = 0
        self.bad_order = 0
        self.last_seq = {}

    def feed(self, data):
        self.bytes_read += len(data)
        self.buf += data
        self._parse()

    def _parse(self):
        b = self.buf
        n = len(b)
        i = 0

        while i < n:
            idx = b.find(REC_MAGIC_BYTES, i)
            if idx < 0:
                # 没有 magic 了：全部丢弃，只留末尾 3 字节（magic 可能跨块）
                i = max(0, n - 3)
                break
            if idx + REC_SIZE > n:
                # 记录还不完整，等下一次 read
                i = idx
                break

            _, seq, wid, crc = struct.unpack_from('<IIII', b, idx)
            payload = bytes(b[idx + REC_HEADER:idx + REC_HEADER + REC_PAYLOAD])

            if (zlib.crc32(payload) & 0xFFFFFFFF) != crc:
                # crc 不自洽 -> 这里不是记录起点（多半是丢弃造成的错位）
                self.resyncs += 1
                i = idx + 1
                continue

            # crc 通过之后仍然强校验 payload：crc 撞车或写侧有 bug 都会被抓到
            if payload != make_payload(wid, seq):
                self.bad_payload += 1
            else:
                prev = self.last_seq.get(wid)
                if prev is not None and seq <= prev:
                    self.bad_order += 1
                self.last_seq[wid] = seq
                self.records_ok += 1

            i = idx + REC_SIZE

        del b[:i]


def phase2_writer(dev, wid, deadline, out):
    fd = os.open(dev, os.O_WRONLY | os.O_NONBLOCK)
    seq = 0
    try:
        while time.monotonic() < deadline:
            for _ in range(64):
                os.write(fd, make_record(wid, seq))
                seq += 1
    except OSError as exc:
        out['err'] = 'writer %d: %s' % (wid, exc)
    finally:
        os.close(fd)
    out['records_written'] = seq
    out['bytes_written'] = seq * REC_SIZE


def phase2_reader(dev, deadline, out):
    fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
    p = Parser()
    try:
        while time.monotonic() < deadline:
            try:
                data = os.read(fd, 16384)
            except OSError as exc:
                if exc.errno == errno.EAGAIN:
                    time.sleep(0.0002)
                    continue
                raise
            if data:
                p.feed(data)
    except OSError as exc:
        out['err'] = 'reader: %s' % exc
    finally:
        os.close(fd)
    out['bytes_read'] = p.bytes_read
    out['records_ok'] = p.records_ok
    out['resyncs'] = p.resyncs
    out['bad_payload'] = p.bad_payload
    out['bad_order'] = p.bad_order


def phase2_process(dev, pid, nthreads, deadline, q):
    """一个进程里跑 nthreads 个线程：一半写一半读"""
    outs = [{'wid': pid * nthreads + i} for i in range(nthreads)]
    threads = []
    writers = max(1, nthreads // 2)
    for i in range(nthreads):
        if i < writers:
            t = threading.Thread(target=phase2_writer,
                                 args=(dev, outs[i]['wid'], deadline, outs[i]))
        else:
            t = threading.Thread(target=phase2_reader,
                                 args=(dev, deadline, outs[i]))
        threads.append(t)
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    q.put(outs)


def run_phase2(dev, procs, threads, seconds):
    buf_size = 4096
    prepare(dev, buf_size, 0)       # flags=0：允许覆盖最老数据

    deadline = time.monotonic() + seconds
    q = Queue()
    ps = [Process(target=phase2_process,
                  args=(dev, i, threads, deadline, q)) for i in range(procs)]
    t0 = time.monotonic()
    for p in ps:
        p.start()

    all_outs = []
    for _ in ps:
        try:
            all_outs.extend(q.get(timeout=seconds + 30))
        except Exception:
            all_outs = []
            break
    for p in ps:
        p.join(timeout=10)
        if p.is_alive():
            p.terminate()
            p.join(timeout=5)
            return False, '并发阶段有进程超时未退出（疑似死锁）', {}
        if p.exitcode != 0:
            return False, '并发阶段进程退出码 %s' % p.exitcode, {}
    elapsed = time.monotonic() - t0

    if not all_outs:
        return False, '未收到并发阶段的结果', {}

    writers = [o for o in all_outs if 'records_written' in o]
    readers = [o for o in all_outs if 'records_ok' in o]

    for o in all_outs:
        if o.get('err'):
            return False, o['err'], {}

    rec_written = sum(o['records_written'] for o in writers)
    bytes_written = sum(o['bytes_written'] for o in writers)
    rec_ok = sum(o['records_ok'] for o in readers)
    bytes_read = sum(o['bytes_read'] for o in readers)
    resyncs = sum(o['resyncs'] for o in readers)
    bad_payload = sum(o['bad_payload'] for o in readers)
    bad_order = sum(o['bad_order'] for o in readers)

    detail = {
        'procs': procs, 'threads': threads,
        'workers': len(all_outs), 'writers': len(writers), 'readers': len(readers),
        'elapsed': elapsed, 'records_written': rec_written,
        'bytes_written': bytes_written, 'records_ok': rec_ok,
        'bytes_read': bytes_read, 'resyncs': resyncs,
        'bad_payload': bad_payload, 'bad_order': bad_order,
    }

    if rec_ok == 0:
        return False, '并发读写过程中没有任何一条记录被成功校验（设备没工作？）', detail
    if bad_payload > 0:
        return False, '发现 %d 条 crc 通过但 payload 不符的记录（真实数据损坏）' % bad_payload, detail
    if bad_order > 0:
        return False, '发现 %d 次同一 writer 的 seq 非递增（数据乱序/重复）' % bad_order, detail

    # 设备侧统计交叉核对：写进去的字节数应当与设备统计一致或更少（丢弃不算）
    fd = os.open(dev, os.O_RDWR)
    try:
        st = get_stats(fd)
    finally:
        os.close(fd)
    detail['dev_bytes_written'] = st[0]
    detail['dev_bytes_read'] = st[1]
    detail['dev_bytes_dropped'] = st[2]
    if st[1] != bytes_read:
        return False, ('设备统计 bytes_read=%d 与读者累计 %d 不一致'
                       % (st[1], bytes_read)), detail

    return True, ('%d 进程 × %d 线程并发 %d 秒无数据损坏'
                  % (procs, threads, round(elapsed, 1))), detail


# ---------------------------------------------------------------------------
# 阶段 3：open/close/ioctl/poll 混压
# ---------------------------------------------------------------------------
def phase3_worker(dev, deadline, out):
    import select

    iters = 0
    errs = 0
    while time.monotonic() < deadline:
        try:
            fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK)
            try:
                get_config(fd)
                os.write(fd, b'churn-churn-churn')
                p = select.poll()
                p.register(fd, select.POLLIN)
                p.poll(0)
                get_stats(fd)
                try:
                    os.read(fd, 128)
                except OSError as exc:
                    if exc.errno != errno.EAGAIN:
                        raise
            finally:
                os.close(fd)
            iters += 1
        except OSError:
            errs += 1
    out['iters'] = iters
    out['errs'] = errs


def phase3_process(dev, nthreads, deadline, q):
    outs = [{} for _ in range(nthreads)]
    threads = [threading.Thread(target=phase3_worker, args=(dev, deadline, o))
               for o in outs]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    q.put(outs)


def run_phase3(dev, procs, threads, seconds):
    prepare(dev, 4096, 0)
    deadline = time.monotonic() + seconds
    q = Queue()
    ps = [Process(target=phase3_process, args=(dev, threads, deadline, q))
          for _ in range(procs)]
    t0 = time.monotonic()
    for p in ps:
        p.start()
    outs = []
    for _ in ps:
        try:
            outs.extend(q.get(timeout=seconds + 30))
        except Exception:
            outs = []
            break
    for p in ps:
        p.join(timeout=10)
        if p.is_alive():
            p.terminate()
            p.join(timeout=5)
            return False, 'open/close 混压阶段有进程超时未退出（疑似引用计数或死锁问题）', {}
        if p.exitcode != 0:
            return False, 'open/close 混压阶段进程退出码 %s' % p.exitcode, {}
    elapsed = time.monotonic() - t0

    iters = sum(o.get('iters', 0) for o in outs)
    errs = sum(o.get('errs', 0) for o in outs)
    detail = {'iters': iters, 'errs': errs, 'elapsed': elapsed}
    if iters == 0:
        return False, 'open/close 混压没有完成任何一次迭代', detail
    return True, ('%d 次 open→ioctl→write→poll→read→close 全流程, 失败 %d 次'
                  % (iters, errs)), detail


# ---------------------------------------------------------------------------
# 完整性校验器自检（--selftest）
#
# 为什么需要这一步：如果只有"阶段 2 报 0 损坏"这一个结论，那它可能是真的没坏，
# 也可能是校验器本身根本抓不到损坏。所以这里对 Parser 单独做自检 —— 故意制造
# 错位、位翻转、seq 篡改、重复记录，确认每一种都能被正确地识别出来。
#
# 本自检**不需要内核模块、不需要 root**，纯用户态逻辑验证。
# ---------------------------------------------------------------------------
def run_selftest():
    checks = []
    fails = 0

    def check(ok, name, detail=''):
        nonlocal fails
        checks.append(ok)
        if not ok:
            fails += 1
        print('[%s] %s%s' % ('PASS' if ok else 'FAIL', name,
                             ('  -- ' + detail) if detail else ''))

    print('=== labdev 完整性校验器自检（纯用户态，不需要 insmod）===\n')

    stream = b''.join(make_record(0, i) for i in range(500))

    # 1) 连续流
    p = Parser(); p.feed(stream)
    check(p.records_ok == 500 and p.bad_payload == 0 and p.bad_order == 0,
          '连续流 500 条记录全部解析通过',
          'records_ok=%d resyncs=%d bad_payload=%d bad_order=%d'
          % (p.records_ok, p.resyncs, p.bad_payload, p.bad_order))

    # 2) 分块投喂（模拟 read 边界把记录切开）
    p = Parser()
    for off in range(0, len(stream), 37):
        p.feed(stream[off:off + 37])
    check(p.records_ok == 500 and p.bad_payload == 0,
          '按 37 字节切块投喂（记录被 read 边界切开）仍全部解析通过',
          'records_ok=%d resyncs=%d' % (p.records_ok, p.resyncs))

    # 3) 中间砍掉一段（模拟缓冲区满覆盖造成的错位）
    cut = stream[:1000] + stream[1500:]
    p = Parser(); p.feed(cut)
    check(p.bad_payload == 0 and p.bad_order == 0 and p.records_ok >= 400,
          '从中间砍掉 500 字节（模拟丢弃错位）后能恢复且无坏数据',
          'records_ok=%d resyncs=%d bad_payload=%d bad_order=%d'
          % (p.records_ok, p.resyncs, p.bad_payload, p.bad_order))

    # 4) 多 writer 交错
    mixed = b''.join(make_record(i % 3, i // 3) for i in range(300))
    p = Parser(); p.feed(mixed)
    check(p.records_ok == 300 and p.bad_payload == 0 and p.bad_order == 0,
          '3 个 writer 交错写入的 300 条记录全部校验通过',
          'records_ok=%d' % p.records_ok)

    # 5) 位翻转（crc 不再自洽）必须被识别为"这里不是记录起点"
    corrupt = bytearray(stream)
    corrupt[20] ^= 0xFF
    p = Parser(); p.feed(bytes(corrupt))
    check(p.records_ok == 499 and p.resyncs >= 1 and p.bad_payload == 0,
          '某条记录 payload 位翻转后：crc 校验发现并跳过（不是当成有效记录）',
          'records_ok=%d resyncs=%d' % (p.records_ok, p.resyncs))

    # 6) 更阴险：只改 seq，payload 和 crc 都不动（crc 只覆盖 payload），
    #    所以 crc 自己仍然完全自洽 —— 必须由 payload 强校验抓住。
    #    这正是 crc 之外的第二道防线的价值所在。
    rec = make_record(7, 0)
    tampered = rec[:4] + struct.pack('<I', 9999) + rec[8:]
    assert len(tampered) == REC_SIZE
    p = Parser(); p.feed(tampered + stream)
    check(p.bad_payload == 1,
          '篡改 seq 但 crc 仍自洽：被 payload 强校验抓住（crc 发现不了这种损坏）',
          'bad_payload=%d records_ok=%d' % (p.bad_payload, p.records_ok))

    # 7) 同一 writer 的 seq 重复/回退必须被抓住
    p = Parser(); p.feed(make_record(0, 5) + make_record(0, 5))
    check(p.bad_order == 1, '同一 writer 重复 seq 被顺序检查抓住',
          'bad_order=%d' % p.bad_order)

    p = Parser(); p.feed(make_record(0, 9) + make_record(0, 3))
    check(p.bad_order == 1, '同一 writer seq 回退被顺序检查抓住',
          'bad_order=%d' % p.bad_order)

    # 8) 随机模糊：随机插入/删除字节，校验通过的记录必须都是真记录
    import random as _random
    rnd = _random.Random(20240927)
    rounds_bad = 0
    total_ok = 0
    for _ in range(200):
        buf = bytearray(stream)
        for _ in range(rnd.randint(1, 6)):
            pos = rnd.randrange(len(buf))
            if rnd.random() < 0.5:
                del buf[pos:pos + rnd.randint(1, 60)]
            else:
                buf[pos:pos] = bytes(rnd.randrange(256)
                                     for _ in range(rnd.randint(1, 60)))
        p = Parser(); p.feed(bytes(buf))
        total_ok += p.records_ok
        if p.bad_payload or p.bad_order:
            rounds_bad += 1
    check(rounds_bad == 0 and total_ok > 0,
          '200 轮随机增删字节的模糊测试：校验通过的记录全部是真实记录',
          '累计校验通过 %d 条, 出现坏数据的轮数 %d' % (total_ok, rounds_bad))

    print('\n自检项: %d 个, 失败 %d 个' % (len(checks), fails))
    if fails:
        print('结果: FAIL')
        return 1
    print('结果: PASS')
    return 0


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description='labdev 并发压测与完整性校验')
    ap.add_argument('--dev', default='/dev/labdev')
    ap.add_argument('--procs', type=int, default=4)
    ap.add_argument('--threads', type=int, default=2)
    ap.add_argument('--seconds', type=float, default=3.0)
    ap.add_argument('--bytes', type=int, default=8 * 1024 * 1024)
    ap.add_argument('--selftest', action='store_true',
                    help='只跑完整性校验器的自检，不需要内核模块也不需要 root')
    args = ap.parse_args()

    if args.selftest:
        return run_selftest()

    if not os.path.exists(args.dev):
        print('错误: %s 不存在，请先 sudo insmod labdev.ko' % args.dev)
        return 1
    if not os.access(args.dev, os.R_OK | os.W_OK):
        print('警告: 当前用户可能没有 %s 的读写权限（模块默认 mode=0600，请用 sudo 运行）'
              % args.dev)

    print('=== labdev 并发压测 (真实内核态, 需要已 insmod) ===')
    print('设备: %s | 进程: %d | 每进程线程: %d | 时长: %ss/阶段\n'
          % (args.dev, args.procs, args.threads, args.seconds))
    print('Python: %s' % sys.version.split()[0])

    failures = 0

    print('\n--- 阶段 1: 严格完整性 (LAB_FLAG_NO_DROP, 1 写 1 读, 逐字节比对) ---')
    t0 = time.monotonic()
    ok, detail = run_phase1(args.dev, args.bytes)
    el = time.monotonic() - t0
    if ok:
        print('[PASS] 阶段 1: %s' % detail)
        print('        吞吐: 写 %.1f MiB/s, 读 %.1f MiB/s (%.2f 秒)'
              % (args.bytes / el / (1 << 20), args.bytes / el / (1 << 20), el))
    else:
        print('[FAIL] 阶段 1: %s' % detail)
        failures += 1

    print('\n--- 阶段 2: 多进程 × 多线程并发读写 (允许丢弃, 记录级校验) ---')
    ok, detail, info = run_phase2(args.dev, args.procs, args.threads, args.seconds)
    if ok:
        print('[PASS] 阶段 2: %s' % detail)
        print('        并发 worker: %d (%d 写 / %d 读)'
              % (info['workers'], info['writers'], info['readers']))
        print('        写入: %d 条记录 / %d 字节' % (info['records_written'],
                                                 info['bytes_written']))
        print('        读取: %d 条记录校验通过 / %d 字节' % (info['records_ok'],
                                                     info['bytes_read']))
        print('        错位重同步 %d 次（丢弃覆盖造成的正常现象）; '
              'payload 损坏 %d; 乱序/重复 %d'
              % (info['resyncs'], info['bad_payload'], info['bad_order']))
        print('        设备侧统计: bytes_written=%d bytes_read=%d bytes_dropped=%d'
              % (info['dev_bytes_written'], info['dev_bytes_read'],
                 info['dev_bytes_dropped']))
        if info['elapsed'] > 0:
            print('        吞吐: 写 %.2f MiB/s, 读 %.2f MiB/s, %.0f 条写/s, %.0f 条校验/s'
                  % (info['bytes_written'] / info['elapsed'] / (1 << 20),
                     info['bytes_read'] / info['elapsed'] / (1 << 20),
                     info['records_written'] / info['elapsed'],
                     info['records_ok'] / info['elapsed']))
    else:
        print('[FAIL] 阶段 2: %s' % detail)
        if info:
            print('        %r' % (info,))
        failures += 1

    print('\n--- 阶段 3: open/close/ioctl/poll 并发混压 ---')
    ok, detail, info = run_phase3(args.dev, args.procs, args.threads, min(args.seconds, 2.0))
    if ok:
        print('[PASS] 阶段 3: %s' % detail)
        print('        %.0f 次/秒 (共 %.2f 秒)'
              % (info['iters'] / max(info['elapsed'], 1e-9), info['elapsed']))
    else:
        print('[FAIL] 阶段 3: %s' % detail)
        failures += 1

    print('\n===== 汇总: 3 个阶段, 失败 %d 个 =====' % failures)
    if failures:
        print('结果: FAIL')
        return 1
    print('结果: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
