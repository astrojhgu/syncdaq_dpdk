#!/usr/bin/env python3
"""分析 fold_pps 的折叠快照，在 N 个点里定位并显示 PPS 窄脉冲。

快照格式：裸 f32 交错 I,Q,I,Q…，N = 文件字节数 / 8（每点 2 个 f32）。

核心难点是 N 可以到 3.2e8，而 PPS 边沿只有几百点宽——直接画图会被压成一个像素、
用均值降采样会被抹平。所以这里的做法是：
  1. 分块算幅度，避免一次性读入 2.56 GB I/Q；
  2. 解算边沿用逐点阈值，不用降采样后的数据；
  3. 全局概览用「每像素取 max」降采样，窄脉冲才留得住。

用法：
  ./analyze_fold.py /tmp/fold.bin                     单快照：概览图 + 每个边沿特写
  ./analyze_fold.py a.bin b.bin c.bin --drift         多快照：对比边沿位置，拟合漂移
"""

import argparse
import os
import sys

import numpy as np

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CHUNK_PTS = 20_000_000        # 每次处理的点数（80 MB I/Q，128 MB 幅度）


def load_amp(path, n_pts=None, chunk=CHUNK_PTS):
    """分块计算幅度 |I+jQ|，返回 f32 数组。"""
    size = os.path.getsize(path)
    if size % 8:
        raise SystemExit(f"{path}: 大小 {size} 不是 8 的倍数，不是 I/Q f32 交错格式")
    total = size // 8
    if n_pts is None:
        n_pts = total
    elif n_pts > total:
        raise SystemExit(f"{path}: 只有 {total} 点，要 {n_pts} 点")

    mm = np.memmap(path, dtype="<f4", mode="r")
    amp = np.empty(n_pts, dtype=np.float32)
    for s in range(0, n_pts, chunk):
        e = min(s + chunk, n_pts)
        b = mm[s * 2 : e * 2].reshape(-1, 2)
        i = b[:, 0].astype(np.float64)
        q = b[:, 1].astype(np.float64)
        amp[s:e] = np.sqrt(i * i + q * q)
    return amp


def find_edges(amp, thr, min_gap=1000):
    """找出幅度超过 thr 的连续段。返回 [(center, width, peak, lo, hi), ...]。"""
    pos = np.flatnonzero(amp > thr)
    if not len(pos):
        return []
    brk = np.flatnonzero(np.diff(pos) > min_gap)
    st = np.r_[0, brk + 1]
    en = np.r_[brk, len(pos) - 1]
    out = []
    for a0, a1 in zip(st, en):
        lo, hi = int(pos[a0]), int(pos[a1])
        seg = amp[lo : hi + 1]
        # 漂移测量必须用真实峰值位置，不能用段几何中心：
        # AC 耦合的振铃让边沿严重不对称，段中心会随形状变化而漂移。
        pk = int(np.argmax(seg)) + lo
        out.append((pk, hi - lo + 1, float(seg.max()), lo, hi))
    return out


def bin_max(amp, width):
    """把序列压缩到 width 个像素，每像素取 max。"""
    factor = max(1, len(amp) // width)
    m = len(amp) // factor * factor
    return amp[:m].reshape(-1, factor).max(axis=1), factor


def plot_overview(amp, edges, out, n_pts, fs, width=2400):
    """全局概览：max 降采样后的幅度曲线，标出所有边沿。"""
    binned, factor = bin_max(amp, width)
    x = np.arange(len(binned)) * factor
    fig, ax = plt.subplots(figsize=(15, 4.5))
    ax.plot(x, binned, lw=0.6, color="#1f77b4")
    ax.set_yscale("log")
    ax.set_xlabel("fold phase (sample index)")
    ax.set_ylabel("amplitude |I+jQ|  (per-pixel max)")
    ax.set_title(
        f"folded overview: {n_pts:,} pts @ {fs/1e6:.0f} MSps, "
        f"1 px = {factor:,} pts, {len(edges)} edge(s)"
    )
    for k, (c, w, pk, lo, hi) in enumerate(edges):
        ax.axvline(c, color="tab:red", lw=0.8, alpha=0.7)
        ax.annotate(
            f"E{k}\n{c:,}\n{w} pts",
            xy=(c, pk),
            xytext=(0, 18),
            textcoords="offset points",
            ha="center",
            fontsize=7,
            color="tab:red",
        )
    ax.grid(alpha=0.25)
    fig.tight_layout()
    fig.savefig(out, dpi=140)
    plt.close(fig)
    return factor


def plot_edge(amp, k, edge, out, fs, half=2000):
    """单个边沿的局部特写，横轴为绝对相位。"""
    c, w, pk, lo, hi = edge
    a = max(0, c - half)
    b = min(len(amp), c + half)
    x = np.arange(a, b)
    fig, ax = plt.subplots(figsize=(11, 4))
    ax.plot(x, amp[a:b], lw=0.9, color="#1f77b4")
    ax.axvline(c, color="gray", ls=":", lw=0.8)
    ax.axhline(pk * 0.01, color="tab:orange", ls="--", lw=0.8, label="1% of peak")
    ax.set_xlabel("fold phase (sample index)")
    ax.set_ylabel("amplitude |I+jQ|")
    ax.set_title(
        f"edge E{k}: peak_pos={c:,}  width={w} pts ({w/fs*1e9:.0f} ns)  peak={pk:.3e}"
    )
    ax.legend(fontsize=8)
    ax.grid(alpha=0.25)
    fig.tight_layout()
    fig.savefig(out, dpi=140)
    plt.close(fig)


def analyze_single(path, args):
    amp = load_amp(path, args.n)
    n_pts = len(amp)
    peak = float(amp.max())
    thr = peak * args.threshold
    if thr <= 0:
        raise SystemExit("阈值不能为 0")
    edges = find_edges(amp, thr, args.min_gap)

    print(f"\n{path}")
    print(f"  点数 {n_pts:,} = {n_pts/args.fs:.6f} s @ {args.fs/1e6:.0f} MSps")
    print(f"  幅度 max={peak:.4e} mean={float(amp.mean()):.4e} 阈值={thr:.4e}")
    print(f"  边沿 {len(edges)} 个")
    for k, (c, w, pk, lo, hi) in enumerate(edges):
        print(
            f"    E{k}: peak_pos={c:,}  width={w} pts ({w/args.fs*1e9:.1f} ns)  "
            f"peak={pk:.4e}  [{lo:,} .. {hi:,}]"
        )

    outdir = args.out or os.path.dirname(os.path.abspath(path))
    os.makedirs(outdir, exist_ok=True)
    base = os.path.splitext(os.path.basename(path))[0]

    factor = plot_overview(
        amp, edges, os.path.join(outdir, f"{base}_overview.png"), n_pts, args.fs
    )
    print(f"  概览图 -> {os.path.join(outdir, base + '_overview.png')} (1 px = {factor:,} 点)")
    for k, e in enumerate(edges):
        p = os.path.join(outdir, f"{base}_edge{k}.png")
        plot_edge(amp, k, e, p, args.fs, args.edge_half)
        print(f"  边沿 E{k} -> {p}")
    return edges


def analyze_drift(paths, args):
    """多个快照对比：同名边沿的相位对时间（或序号）拟合斜率。"""
    series = []
    n_pts_ref = None
    for p in paths:
        amp = load_amp(p, args.n)
        n_pts_ref = len(amp)
        peak = float(amp.max())
        edges = find_edges(amp, peak * args.threshold, args.min_gap)
        if not edges:
            print(f"{p}: 没找到边沿，跳过")
            continue
        # 取最强的边沿作为参照
        edges.sort(key=lambda e: -e[2])
        series.append((p, edges[0][0], len(edges)))
        print(f"{p}: 最强边沿 peak_pos={edges[0][0]:,}  width={edges[0][1]}  (共 {len(edges)} 个)")

    if len(series) < 2:
        print("\n少于两个快照，无法拟合漂移")
        return

    idx = np.arange(len(series), dtype=np.float64)
    cents = np.array([s[1] for s in series], dtype=np.float64)
    # 最小二乘斜率（点/快照序号）
    A = np.vstack([idx, np.ones_like(idx)]).T
    slope, intercept = np.linalg.lstsq(A, cents, rcond=None)[0]
    residuals = cents - (A @ np.array([slope, intercept]))
    per_sec = slope / args.snapshot_sec
    n_ref = n_pts_ref if n_pts_ref else int(args.n or 0)
    ppm = per_sec / n_ref * 1e6 if n_ref else 0.0
    print(f"\n线性拟合：斜率 {slope:+.3f} 点/快照（残差 rms {float(np.sqrt((residuals**2).mean())):.2f} 点）")
    print(f"  假设快照间隔 {args.snapshot_sec} s：{per_sec:+.4f} 点/秒")
    print(f"  折算漂移率：{ppm:+.4f} ppm（相对 N={n_ref:,}）")
    if abs(ppm) > 0:
        print(f"  累积 1 小时将偏移 {per_sec*3600:+.1f} 点")

    fig, ax = plt.subplots(figsize=(9, 4.5))
    ax.plot(idx, cents, "o-", ms=4)
    ax.plot(idx, A @ np.array([slope, intercept]), "--", lw=1, label="linear fit")
    ax.set_xlabel("snapshot index")
    ax.set_ylabel("edge peak position (fold phase)")
    ax.set_title(f"edge drift: {ppm:+.4f} ppm")
    ax.legend(fontsize=8)
    ax.grid(alpha=0.25)
    out = args.out or "."
    os.makedirs(out, exist_ok=True)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "drift.png"), dpi=140)
    print(f"  漂移图 -> {os.path.join(out, 'drift.png')}")


def main():
    ap = argparse.ArgumentParser(
        description="分析 fold_pps 快照，在超长序列里定位 PPS 窄脉冲"
    )
    ap.add_argument("files", nargs="+", help="快照文件（裸 f32 交错 I/Q）")
    ap.add_argument("--n", type=int, default=None, help="只分析前 n 点（默认全部）")
    ap.add_argument("--fs", type=float, default=320e6, help="采样率 Hz")
    ap.add_argument("--threshold", type=float, default=0.01, help="边沿阈值 = max × 该系数")
    ap.add_argument("--min-gap", type=int, default=1000, help="两段之间至少隔多少点才算不同边沿")
    ap.add_argument("--edge-half", type=int, default=2000, help="边沿特写的半幅（点）")
    ap.add_argument("--out", default=None, help="输出目录（默认与快照同目录）")
    ap.add_argument("--drift", action="store_true", help="多快照对比并拟合漂移")
    ap.add_argument("--snapshot-sec", type=float, default=30.0, help="快照间隔秒数（用于漂移换算）")
    args = ap.parse_args()

    for p in args.files:
        if not os.path.exists(p):
            raise SystemExit(f"找不到 {p}")

    if args.drift:
        analyze_drift(args.files, args)
    else:
        for p in args.files:
            analyze_single(p, args)


if __name__ == "__main__":
    main()
