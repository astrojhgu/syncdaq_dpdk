#!/usr/bin/env bash
# 启动 pktloss。必须 root（mlx5 PMD 需要 CAP_NET_ADMIN/CAP_NET_RAW + 大页）。
#
# 数据流由调用者控制，本脚本不发 StreamStart/StreamStop。正确顺序：
#
#   1. 板卡 StreamStop，等 >=4 s（PPS 生效）
#   2. sudo ./run.sh --chans 0-7 [-d N]
#   3. 看到 "计划运行" / "一直运行" 之后再 StreamStart
#   4. 测完 StreamStop
#
# 反过来（板卡已经在发时直接起接收）也能测，但第一秒会集中丢 ~41 k 个包
# （约 0.26 s 的数据，全记在 ch0 上）：flow 规则是 dev_start 之后才建的，
# 那段时间的包进不了 DPDK 队列。起接收时会检测板卡状态并警告。
#
#   sudo ./run.sh                          # 默认 8 路，跑到 Ctrl-C
#   sudo ./run.sh --chans 0-3 -d 20        # 只收 4 路，跑 20 秒
#   sudo ./run.sh --chans 0,2,4,6 --csv /tmp/loss.csv -d 60
#
# 环境变量：PCI（默认 0000:01:00.0）、LCORES（默认 0-8）、SOCKET_MEM（默认 1536 MB）、
#           DPDK_LOG（默认 lib.eal:warning）、RT_PRIO（默认 0，0 = 不设 SCHED_FIFO）
#
# 注意：DPDK 独占数据口，运行期间内核侧 eno1np0 / 10.100.11.1 收不到包；
#       用户态 recv_perf 之类的对照工具在这段时间会数到 0，这是正常的。
set -euo pipefail
cd "$(dirname "$0")"

BIN=$PWD/pktloss
PCI=${PCI:-0000:01:00.0}
LCORES=${LCORES:-0-8}
SOCKET_MEM=${SOCKET_MEM:-1536}      # MB
DPDK_LOG=${DPDK_LOG:-lib.eal:warning}

[ -x "$BIN" ] || { echo "还没编译：make" >&2; exit 1; }

free_hp=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages)
need_hp=$((SOCKET_MEM / 2))
stale=$(find /dev/hugepages -maxdepth 1 -name "pktlossmap_*" 2>/dev/null | wc -l)
# 上次异常退出（被 kill / 崩溃）会在 /dev/hugepages 留下文件，页被占着。
# --huge-unlink 会让 EAL 复用后立即 unlink，退出即释放，所以这里把它们算作可回收。
if [ $((free_hp + stale)) -lt "$need_hp" ]; then
    echo "2M 大页空闲 $free_hp + 残留 $stale 个，需要 $need_hp 个（--socket-mem ${SOCKET_MEM} MB）" >&2
    echo "加页： sudo dpdk-hugepages.py -p 2M -r 4G   或   sudo ./setup.sh PAGES=2048" >&2
    exit 1
fi
if [ "$stale" -gt 0 ]; then
    echo "提示: 发现 $stale 个上次残留的大页文件，--huge-unlink 会回收它们"
fi

# 板卡是否正在发流：读内核侧 rx_packets 的 1 秒增量。DPDK 没起时这个计数
# 有效；正在发的话是 ~1.25 Mpps。只看，不改板卡状态。
IFACE=${IFACE:-$(ls "/sys/bus/pci/devices/$PCI/net" 2>/dev/null | head -1 || true)}
STREAMING=0
if [ -n "$IFACE" ]; then
    rxa=$(cat "/sys/class/net/$IFACE/statistics/rx_packets" 2>/dev/null || echo 0)
    sleep 1
    rxb=$(cat "/sys/class/net/$IFACE/statistics/rx_packets" 2>/dev/null || echo 0)
    if [ $((rxb - rxa)) -gt 100000 ]; then
        STREAMING=$((rxb - rxa))
    fi
fi

cat <<EOF
== pktloss ==
设备 $PCI   lcore $LCORES   大页 ${SOCKET_MEM} MB（空闲 $free_hp 个 2M 页）
EOF

if [ "$STREAMING" -gt 0 ]; then
    cat <<EOF
板卡正在发流（${STREAMING} pps）。flow 规则生效前积压的残留包会被丢掉，
统计从之后的第一个包开始，所以首包编号不是 0（不影响丢包率）。
要让首包从 0 开始：板卡 StreamStop → 等 4 s → 重起本脚本 → 看到下面
两行之一后再 StreamStart。
EOF
else
    echo "板卡当前没有数据流。看到下面两行之一后再发 StreamStart："
fi
cat <<EOF
      "计划运行 N s；Ctrl-C 可提前结束并打印汇总。"
      "一直运行，Ctrl-C 结束并打印汇总。"
      Ctrl-C 结束时打印汇总。
EOF

RT_PRIO=${RT_PRIO:-0}           # worker 的 SCHED_FIFO 优先级；0 = 不设（默认）
                                # 要用 RT 必须先关 sched_rt_runtime_us，否则会周期性丢包
EAL_ARGS=(-l "$LCORES" -n 4 -a "$PCI"
          --socket-mem "$SOCKET_MEM"
          --file-prefix pktloss
          --huge-unlink
          --log-level="$DPDK_LOG")

# 把 mlx5 的中断从 worker 核（1-8）挪到 9-15。DPDK 是轮询收包，这些中断只会
# 打断 worker；非目标流量（走默认 RSS）和链路事件触发时尤其明显。
# 只有 root 能写 /proc/irq/*/smp_affinity，所以用 sudo 跑时才生效。
if [ "$(id -u)" -eq 0 ] && [ -x "$PWD/tune.sh" ]; then
    "$PWD/tune.sh" >/dev/null 2>&1 || echo "提示: 中断迁移失败（继续启动）"
fi

if [ "$(id -u)" -eq 0 ]; then
    exec "$BIN" "${EAL_ARGS[@]}" -- --sched-fifo "$RT_PRIO" "$@"
else
    exec sudo "$BIN" "${EAL_ARGS[@]}" -- --sched-fifo "$RT_PRIO" "$@"
fi
