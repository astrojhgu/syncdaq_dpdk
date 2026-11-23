#!/usr/bin/env bash
# 把 mlx5 的中断从收包核（lcore 1-8）挪到不跑 worker 的核（9-15）。
#
#   sudo ./tune.sh                 # 默认 worker 1-8，中断落 9-15
#   sudo WORKERS=1-4 TARGET=5-15 ./tune.sh
#
# 为什么：DPDK 收包是轮询，不需要这些中断，但链路状态/异步事件/错误上报的
# MSI-X 仍会打在 worker 核上。一次中断处理足够让 worker 停几十微秒，配合
# 网卡侧偶发突发就可能顶掉队列。把它们挪到 idle 核是零成本的改善。
#
# 注意：这只解决"中断打断"。真正要隔离 worker 核，需要内核启动参数
#   isolcpus=1-8 nohz_full=1-8 rcu_nocbs=1-8
# 改 /etc/default/grub 后 update-grub && reboot。
set -euo pipefail

[ "$(id -u)" -eq 0 ] || { echo "需要 root：sudo ./tune.sh" >&2; exit 1; }

WORKERS=${WORKERS:-1-8}
TARGET=${TARGET:-9-15}
IFACE=${IFACE:-eno1np0}

# 目标核 → smp_affinity 掩码
mask=$(python3 - "$TARGET" <<'PY'
import sys
spec = sys.argv[1]
bits = 0
for part in spec.split(','):
    if '-' in part:
        a, b = part.split('-')
        for c in range(int(a), int(b) + 1):
            bits |= 1 << c
    else:
        bits |= 1 << int(part)
print(f"{bits:x}")
PY
)
echo "worker 核: $WORKERS   中断目标: $TARGET (mask=0x$mask)   接口: $IFACE"

# 找出属于该设备的 MSI-X 中断号：/proc/interrupts 里 mlx5 的行
irqs=$(awk '/mlx5/ { gsub(/:/, "", $1); print $1 }' /proc/interrupts | sort -n | uniq)

if [ -z "$irqs" ]; then
    echo "没找到 $IFACE 的中断（设备可能未加载驱动）" >&2
    exit 1
fi

for n in $irqs; do
    [ -w "/proc/irq/$n/smp_affinity" ] || continue
    echo "$mask" > "/proc/irq/$n/smp_affinity" 2>/dev/null || continue
    echo "  irq $n → $(cat "/proc/irq/$n/smp_affinity_list" 2>/dev/null)"
done

echo
echo "当前中断分布（应全部落在 $TARGET）："
grep mlx5 /proc/interrupts | head -10 | cut -c1-100

echo
echo "提示："
echo "  1. 这只管中断；要真正隔离 CPU，请加内核参数"
echo "     GRUB_CMDLINE_LINUX_DEFAULT=\"... isolcpus=1-8 nohz_full=1-8 rcu_nocbs=1-8\""
echo "     然后 sudo update-grub && sudo reboot"
echo "  2. worker 已默认 SCHED_FIFO 50（run.sh 的 RT_PRIO 可调），比 isolcpus 弱但不用重启"
echo "  3. 重启后中断亲和性会重置，把本脚本放进 rc.local 或 systemd unit"
