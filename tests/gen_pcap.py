#!/usr/bin/env python3
"""生成 pcap 测试流：模拟 syncdaq 板卡 100G 数据面。

线上格式（与 doc/system_notes.typ、src/payload.rs 一致，小端）：
  UDP 载荷 8224 B = 32 B MetaData(head_magic@0, version@4, port_id@8,
  data_type@12, pkt_cnt@16, tail_magic@24) + 8192 B 采样。

覆盖的场景：
  ch0  seq 0,1,2,5,6,6,3,4   —— 正常 / 跳跃 2 / 重复 / 回退（流重启）
  ch0  一条 magic 错误的包     —— 坏头
  ch2  seq 100..109           —— 中途接入（首包非 0）
  ch4  seq 0, 10000           —— 大跳跃 9999
  ch6  seq 0,1                —— 干净
  ch1  seq 0,1,2              —— 不在 --chans 里，应计入"非目标包"
"""

import socket
import struct
import sys

HEAD_MAGIC = 0xAA55AA55
TAIL_MAGIC = 0xAA55AA5544332211
SAMPLE_LEN = 8192

HOST_MAC = bytes.fromhex("1070fdb368de")   # eno1np0
BOARD_MAC = bytes.fromhex("1070fdb360ef")  # 板卡 port0 src_mac
HOST_IP = "10.100.11.1"
BOARD_IP = "10.100.11.20"


def build_frame(dport, seq, magic=HEAD_MAGIC, port_id=None, datatype=16):
    if port_id is None:
        port_id = dport - 4000

    meta = struct.pack("<IIIIQQ", magic, 0x11223344, port_id, datatype, seq,
                       TAIL_MAGIC)
    # 采样内容不重要，只求和长度对；用递增字节便于肉眼辨认
    body = meta + bytes((seq + i) & 0xFF for i in range(SAMPLE_LEN))

    udp = struct.pack("!HHHH", 3001, dport, 8 + len(body), 0)
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0,
                     socket.inet_aton(BOARD_IP), socket.inet_aton(HOST_IP))
    eth = HOST_MAC + BOARD_MAC + b"\x08\x00"
    return eth + ip + udp + body


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/pktloss_test.pcap"

    frames = []
    frames += [(4000, s, HEAD_MAGIC) for s in (0, 1, 2, 5, 6, 6, 3, 4)]
    frames += [(4000, 10, 0xDEADBEEF)]                      # 坏 magic
    frames += [(4002, s, HEAD_MAGIC) for s in range(100, 110)]
    frames += [(4004, 0, HEAD_MAGIC), (4004, 10000, HEAD_MAGIC)]
    frames += [(4006, 0, HEAD_MAGIC), (4006, 1, HEAD_MAGIC)]
    frames += [(4001, s, HEAD_MAGIC) for s in range(3)]      # 非目标端口

    # pcap: 小端 magic, v2.4, snaplen 65535, DLT_EN10MB
    buf = bytearray(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))

    ts_usec = 0
    for dport, seq, magic in frames:
        pkt = build_frame(dport, seq, magic)
        ts_sec, ts_usec = divmod(ts_usec + 50, 1_000_000)
        buf += struct.pack("<IIII", ts_sec, ts_usec, len(pkt), len(pkt)) + pkt

    with open(out_path, "wb") as f:
        f.write(buf)

    print(f"写出 {out_path}: {len(frames)} 帧，{len(buf)} 字节")


if __name__ == "__main__":
    main()
