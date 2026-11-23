#!/usr/bin/env python3
"""解析 pktloss --raw-ch 输出（stride=1 全采样）。

文件格式（每包 8200 B，小端）：
    offset 0    u64  seq（板卡帧编号）
    offset 8    2048 × (int16 I, int16 Q)   即 8192 B 采样，fs 名义 320 MSps

用法：
    ./decode_raw.py /dev/shm/cw4.bin              # 概况 + FFT
    ./decode_raw.py /dev/shm/cw4.bin --phase      # 加相位连续性/包边界检查
    ./decode_raw.py /dev/shm/cw4.bin -n 500       # 只看前 500 包
"""

import argparse
import sys

import numpy as np

REC = 8200          # 8 B seq + 8192 B 采样
FS_NOM = 320e6      # 名义采样率（实际约 317.3，见 README 第 11 节）


def load(path, npkt):
    raw = np.fromfile(path, dtype=np.uint8)
    n = len(raw) // REC
    if n == 0:
        sys.exit("文件太短或格式不符")
    n = min(n, npkt)
    raw = raw[: n * REC].reshape(n, REC)
    seq = raw[:, :8].copy().view("<u8").ravel()
    iq = raw[:, 8:].copy().view("<i2").reshape(n, 2048, 2)
    return seq, iq[:, :, 0].astype(np.float64), iq[:, :, 1].astype(np.float64)


def estimate_freq(c, fs=FS_NOM):
    """用相位拟合迭代估计信号频率（比 FFT 峰值精确）。"""
    n = c.size
    nidx = np.arange(n)
    win = np.hanning(min(n, 65536))
    S = np.abs(np.fft.fft(c[: win.size] * win))
    f0 = np.fft.fftfreq(win.size, d=1 / fs)[int(np.argmax(S))]
    for _ in range(5):
        e = np.convolve(c * np.exp(-2j * np.pi * f0 / fs * nidx), [0.5, 0.5], mode="valid")
        ph = np.unwrap(np.angle(e))
        sl = np.polyfit(np.arange(ph.size), ph, 1)[0]
        f0 += sl * fs / (2 * np.pi)
    return f0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("-n", "--npkt", type=int, default=200, help="分析包数（默认 200）")
    ap.add_argument("--phase", action="store_true", help="做相位连续性/包边界检查")
    a = ap.parse_args()

    seq, I, Q = load(a.path, a.npkt)
    n = len(seq)
    span = int(seq[-1] - seq[0] + 1)
    amp = np.abs(I) + np.abs(Q)
    print(f"文件      : {a.path}")
    print(f"包数      : {n}（seq {seq[0]}..{seq[-1]}，跨度 {span}，丢 {span - n}）")
    print(f"seq 连续  : {span == n}")
    print(f"幅度      : p50={np.percentile(amp,50):.0f} p99={np.percentile(amp,99):.0f} "
          f"max={amp.max():.0f}（满量程 32768）")

    c = (I + 1j * Q).reshape(-1)
    c = c - c.mean()
    N = c.size
    S = np.abs(np.fft.fft(c * np.hanning(N)))
    fr = np.fft.fftfreq(N, d=1 / FS_NOM)
    k = int(np.argmax(S))
    print(f"FFT 峰    : {fr[k]/1e6:+.4f} MHz（按 {FS_NOM/1e6:.0f} MSps 解算）  "
          f"SNR={S[k]/np.median(S):.0f}")

    if a.phase:
        f0 = estimate_freq((I + 1j * Q).reshape(-1))
        w = 2 * np.pi * f0 / FS_NOM
        print(f"\n精确频率  : {f0/1e6:+.6f} MHz  → ω={w:.8f} rad/样本（{np.degrees(w):.3f}°）")
        demod = c * np.exp(-1j * w * np.arange(N))
        phr = np.angle(demod)
        jumps = []
        for kk in range(n - 1):
            b = (kk + 1) * 2048
            if b >= len(phr):
                break
            j = phr[b] - phr[b - 1]
            jumps.append((j + np.pi) % (2 * np.pi) - np.pi)
        jumps = np.array(jumps)
        print(f"包边界跳变: mean={jumps.mean():+.5f} std={jumps.std():.5f} "
              f"max|={np.abs(jumps).max():.4f} rad")
        print(f"  丢 1 点应为 {abs(w):.3f} rad；丢 17 点应为 {17*abs(w) % (2*np.pi):.3f} rad")
        dd = np.diff(phr)
        dd = (dd + np.pi) % (2 * np.pi) - np.pi
        print(f"样本间差分: std={dd.std():.5f} rad  |dd|>0.3 占 {(np.abs(dd)>0.3).mean()*100:.4f}%")


if __name__ == "__main__":
    main()
