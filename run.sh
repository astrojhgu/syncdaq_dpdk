#!/usr/bin/env bash
# 启动 pktloss。必须 root（mlx5 PMD 需要 CAP_NET_ADMIN/CAP_NET_RAW + 大页）。
#
#   sudo ./run.sh                          # 默认 8 路，跑到 Ctrl-C
#   sudo ./run.sh --chans 0-3 -d 20        # 只收 4 路，跑 20 秒
#   sudo ./run.sh --chans 0,2,4,6 --csv /tmp/loss.csv -d 60
#
# 环境变量：PCI（默认 0000:01:00.0）、LCORES（默认 0-8）、SOCKET_MEM（默认 1536 MB）、
#           DPDK_LOG（默认 lib.eal:warning）
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

cat <<EOF
== pktloss ==
设备 $PCI   lcore $LCORES   大页 ${SOCKET_MEM} MB（空闲 $free_hp 个 2M 页）
提示：接收端就绪后再让板卡 StreamStart，否则首包编号非 0（程序会以首包为基线，
      不影响丢包率，但看不到"第 0 帧"）。
      Ctrl-C 结束时打印汇总。
EOF

RT_PRIO=${RT_PRIO:-0}           # worker 的 SCHED_FIFO 优先级；0 = 不设（默认）
                                # 要用 RT 必须先关 sched_rt_runtime_us，否则会周期性丢包
EAL_ARGS=(-l "$LCORES" -n 4 -a "$PCI"
          --socket-mem "$SOCKET_MEM"
          --file-prefix pktloss
          --huge-unlink
          --log-level="$DPDK_LOG")

if [ "$(id -u)" -eq 0 ]; then
    exec "$BIN" "${EAL_ARGS[@]}" -- --sched-fifo "$RT_PRIO" "$@"
else
    exec sudo "$BIN" "${EAL_ARGS[@]}" -- --sched-fifo "$RT_PRIO" "$@"
fi
