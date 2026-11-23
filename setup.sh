#!/usr/bin/env bash
# 一次性环境准备 / 检查（root）。幂等：已经满足的项不会重复改。
#
#   sudo ./setup.sh                 # 默认 1024 个 2M 页（2 GiB）
#   sudo PAGES=2048 ./setup.sh      # 4 GiB
#
# mlx5 PMD 走 bifurcated driver，不需要 dpdk-devbind 把设备从内核解绑，
# 所以这里不做 unbind —— 内核侧接口（10.100.11.1）保留，DPDK 退出后即可恢复。
set -euo pipefail

PAGES=${PAGES:-1024}          # 2M 页数量
DEVPAGE=2048
PCI=${PCI:-0000:01:00.0}

die()  { echo "错误: $*" >&2; exit 1; }
info() { echo "[setup] $*"; }

[ "$(id -u)" -eq 0 ] || die "需要 root：sudo ./setup.sh"

# ---------------------------------------------------------------- 大页
cur=$(cat /sys/kernel/mm/hugepages/hugepages-${DEVPAGE}kB/nr_hugepages)
if [ "$cur" -lt "$PAGES" ]; then
    info "2M 大页 $cur → $PAGES（$((${PAGES} * DEVPAGE / 1024)) MiB）"
    echo "$PAGES" > /sys/kernel/mm/hugepages/hugepages-${DEVPAGE}kB/nr_hugepages
else
    info "2M 大页已有 $cur 个（>= $PAGES），跳过"
fi

if ! mountpoint -q /dev/hugepages; then
    info "挂载 hugetlbfs 到 /dev/hugepages"
    mkdir -p /dev/hugepages
    mount -t hugetlbfs nodev /dev/hugepages
fi

# ---------------------------------------------------------------- DPDK
ver=$(pkg-config --modversion libdpdk 2>/dev/null) \
    || die "找不到 libdpdk（apt install dpdk libdpdk-dev）"
info "DPDK $ver"

pmd=/usr/lib/x86_64-linux-gnu/librte_net_mlx5.so
[ -e "$pmd" ] || die "缺 mlx5 PMD（$pmd）—— EAL 会自动从 dpdk/pmds-* 目录加载它"
info "mlx5 PMD: $(readlink -f "$pmd")"

# ---------------------------------------------------------------- 网卡
[ -e "/sys/bus/pci/devices/$PCI" ] || die "找不到 PCI 设备 $PCI（dpdk-devbind.py -s 看地址）"
drv=$(basename "$(readlink -f "/sys/bus/pci/devices/$PCI/driver" 2>/dev/null || echo none)")
info "设备 $PCI 当前驱动: $drv"
case "$drv" in
    mlx5_core) : ;;
    *) echo "[setup] 警告: 该设备不是 mlx5_core，DPDK 侧需要 --allow 的地址对应真实网卡" ;;
esac

ifc=$(ls "/sys/bus/pci/devices/$PCI/net" 2>/dev/null | head -1 || true)
if [ -n "$ifc" ]; then
    info "内核接口 $ifc，MTU $(cat "/sys/class/net/$ifc/mtu")，MAC $(cat "/sys/class/net/$ifc/address")"
    echo "[setup] 注意: DPDK 运行期间该接口的数据面被 DPDK 独占（内核收不到包），退出后恢复"
fi

# ---------------------------------------------------------------- 结果
total=$(grep HugePages_Total /proc/meminfo | awk '{print $2}')
free=$(grep HugePages_Free  /proc/meminfo | awk '{print $2}')
info "大页: total=$total free=$free（2M 页）"
info "完成。接下来: make && ./tests/run_pcap_test.sh（免 root 自测）"
