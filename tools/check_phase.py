#!/usr/bin/env python3
"""检查 raw 采集数据的相位连续性（判包间是否有间隙/丢点）。

支持两种格式：
  capture_pipeline : 每包 8192 B（int16 I/Q 交替，无 seq）
  pktloss --raw-ch : 每包 8200 B（8 B seq + 8192 B 采样）

判据：
  解调后取相位，看「第 k 包最后一点 → 第 k+1 包第一点」的相位差。
  - 无间隙（背靠背采样）      → jump ≈ 0
  - 包间有间隙 t_gap（外部源） → jump = 2π·f_sig·t_gap (mod 2π)
  - 包间丢了 N 个采样点        → jump = N·ω (mod 2π)

  注意：ω 接近 2π·k/N 时会发生 2π 模糊（450MHz@320MSps 就是 17.44 点≈3整圈）。
  所以必须用「ω 使得 N·ω 不接近 2π 整数倍」的频率来测。

用法：
  ./check_phase.py /dev/shm/h400n380_0.dat                  # capture_pipeline 格式
  ./check_phase.py /dev/shm/cw4.bin --seq                    # pktloss 格式（含 seq）
  ./check_phase.py /dev/shm/h400n380_0.dat -n 500 -s 320e6
"""

import argparse
import numpy as np


def load(path, npkt, has_seq):
    rec = 8200 if has_seq else 8192
    raw = np.fromfile(path, dtype=np.uint8)
    n = len(raw) // rec
    n = min(n, npkt)
    a = raw[: n * rec].reshape(n, rec)
    seq = a[:, :8].copy().view('<u8').ravel() if has_seq else np.arange(n, dtype=np.uint64)
    iq = a[:, 8:].copy().view('<i2').reshape(n, 2048, 2) if has_seq \
         else a.view('<i2').reshape(n, 2048, 2)
    return seq, iq[:, :, 0].astype(np.float64), iq[:, :, 1].astype(np.float64)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('-n', '--npkt', type=int, default=200)
    ap.add_argument('-s', '--fs', type=float, default=320e6)
    ap.add_argument('--seq', action='store_true', help='pktloss 格式（每包前 8 B seq）')
    a = ap.parse_args()

    seq, I, Q = load(a.path, a.npkt, a.seq)
    n = len(seq)
    amp = np.abs(I) + np.abs(Q)
    print(f"文件   : {a.path}")
    print(f"包数   : {n}   seq {seq[0]}..{seq[-1]}   跨度 {seq[-1]-seq[0]+1}   缺 {seq[-1]-seq[0]+1-n}")
    print(f"幅度   : p50={np.percentile(amp,50):.0f} p99={np.percentile(amp,99):.0f} "
          f"max={amp.max():.0f}  (满量程 32768)")

    c = (I + 1j * Q).reshape(-1)
    N = c.size
    nidx = np.arange(N)

    # 1) 粗频率：FFT 峰
    w = np.hanning(min(N, 65536))
    S = np.abs(np.fft.fft(c[: w.size] * w))
    f0 = np.fft.fftfreq(w.size, d=1 / a.fs)[int(np.argmax(S))]

    # 2) 迭代精化（短平滑，保留包边界突变）
    for _ in range(6):
        e = np.convolve(c * np.exp(-2j * np.pi * f0 / a.fs * nidx),
                        np.array([0.5, 0.5]), mode='valid')
        ph = np.unwrap(np.angle(e))
        sl = np.polyfit(np.arange(ph.size), ph, 1)[0]
        f0 += sl * a.fs / (2 * np.pi)

    omega = 2 * np.pi * f0 / a.fs
    print(f"基带   : {f0/1e6:+.6f} MHz   ω={omega:+.6f} rad/point ({np.degrees(omega):+.3f}°)")
    print(f"d×32   : {f0/a.fs*32:.6f}")

    # 3) 包边界相位差
    demod = c * np.exp(-1j * omega * nidx)
    phr = np.angle(demod)
    jumps = []
    for k in range(n - 1):
        b = (k + 1) * 2048
        if b >= len(phr):
            break
        j = phr[b] - phr[b - 1]
        jumps.append((j + np.pi) % (2 * np.pi) - np.pi)
    jumps = np.array(jumps)

    # 4) 包内随机位置作对照
    rng = np.random.default_rng(0)
    inner = []
    for k in range(n - 1):
        p = int(rng.integers(10, 2038))
        j = phr[k * 2048 + p + 1] - phr[k * 2048 + p]
        inner.append((j + np.pi) % (2 * np.pi) - np.pi)
    inner = np.array(inner)

    print(f"\n包边界相位差 ({len(jumps)} 处):")
    print(f"  mean={jumps.mean():+.5f}  std={jumps.std():.5f}  "
          f"min={jumps.min():+.4f}  max={jumps.max():+.4f}")
    print(f"  均值标准误 = {jumps.std()/np.sqrt(len(jumps)):.5f} rad")
    print(f"包内随机位置 (对照):")
    print(f"  mean={inner.mean():+.5f}  std={inner.std():.5f}")

    print(f"\n换算缺失点数 (jump / ω):")
    print(f"  mean = {jumps.mean()/omega:+.4f} 点,  "
          f"1σ = {jumps.std()/abs(omega):.4f} 点")

    print(f"\n2π 模糊检查：17.44 点 → {17.44*omega % (2*np.pi):+.4f} rad")
    print(f"  若 ω 接近 2πk/N，跳变会被掩盖")


if __name__ == '__main__':
    main()
