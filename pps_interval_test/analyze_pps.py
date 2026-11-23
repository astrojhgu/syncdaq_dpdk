import numpy as np, os, sys

def analyze(path, fs, bits, label):
    sz = os.path.getsize(path)
    bpp = 8192                                   # 每包字节数
    pts = bpp // (2 * (bits // 8))               # 每包复数点数
    nb = sz // bpp
    n = sz // (2 * (bits // 8))
    print(f"\n=== {label} ===")
    print(f"文件 {sz:,} B = {nb:,} 包 × 8192 B；复数样本 {n:,} = {n/fs:.4f} s")
    print(f"采样率 {fs/1e6:.0f} MSps, {bits}bit, 每包 {pts} 点；1 s 理论 {int(fs):,} 点")
    dt = '<i1' if bits == 8 else '<i2'
    mm = np.memmap(path, dtype=dt, mode='r', shape=(n, 2))
    CH = max(10_000_000, int(fs / 20))
    # 抽样统计
    samp = []
    for s in range(0, n, CH * 5):
        b = mm[s:s + CH].astype(np.float32)
        samp.append(np.sqrt(b[:, 0]**2 + b[:, 1]**2)[::max(1, CH//2000)])
    sample = np.concatenate(samp)
    p50, p999 = np.percentile(sample, 50), np.percentile(sample, 99.9)
    full = 127.0 if bits == 8 else 32767.0
    thr = max(p999 * 5, full * 0.01)
    print(f"幅度 p50={p50:.1f} p99.9={p999:.1f} max={sample.max():.1f} （满量程 {full:.0f}）")
    print(f"脉冲阈值 = {thr:.0f}")
    pos = []
    for s in range(0, n, CH):
        b = mm[s:s + CH].astype(np.float32)
        a = np.sqrt(b[:, 0]**2 + b[:, 1]**2)
        i = np.flatnonzero(a > thr)
        if len(i):
            pos.append(i.astype(np.int64) + s)
    pos = np.concatenate(pos) if pos else np.array([], dtype=np.int64)
    print(f"超阈值样点 {len(pos)}")
    if not len(pos):
        print("  没找到脉冲"); return
    gap_thr = int(fs * 0.01)          # 10 ms
    g = np.diff(pos); brk = np.flatnonzero(g > gap_thr)
    st = np.r_[0, brk + 1]; en = np.r_[brk, len(pos) - 1]
    cen = []
    print(f"脉冲段 {len(st)} 个:")
    for i, (a0, a1) in enumerate(zip(st, en)):
        w = pos[a1] - pos[a0] + 1
        c = (pos[a0] + pos[a1]) / 2
        cen.append(c)
        print(f"  段{i}: start={pos[a0]:,}  end={pos[a1]:,}  宽={w} ({w/fs*1e9:.0f} ns)")
    if len(cen) > 1:
        print("  相邻间隔:")
        for i in range(len(cen) - 1):
            d = cen[i+1] - cen[i]
            print(f"    → {d:>15,.0f} 点 = {d/fs:.9f} s")
    # 同类边沿周期（隔一个）
    if len(cen) > 2:
        print("  同类边沿周期（隔一个脉冲）:")
        for i in range(len(cen) - 2):
            T = cen[i+2] - cen[i]
            print(f"    T{i}→{i+2} = {T:,.0f} 点 = {T/fs:.9f} s  偏差 {(T-fs)/fs*1e6:+.3f} ppm")

if __name__ == '__main__':
    jobs = [
        ('/dev/shm/pps_320.dat',    320e6, 16, '320MSps_iq (16bit)'),
        ('/dev/shm/pps_100.dat',    100e6, 16, '100MSps_iq (16bit)'),
        ('/dev/shm/pps_100_8b.dat', 100e6,  8, '100MSps_iq_8bit (8bit)'),
        ('/dev/shm/pps_640_8b.dat', 640e6,  8, '640MSps_iq (8bit)'),
    ]
    for p, fs, b, lab in jobs:
        if os.path.exists(p) and os.path.getsize(p) > 0:
            analyze(p, fs, b, lab)
        else:
            print(f"\n=== {lab} ===\n  （文件不存在或为空：{p}）")
