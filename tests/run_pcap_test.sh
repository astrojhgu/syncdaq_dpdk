#!/usr/bin/env bash
# 无特权端到端自测：用 pcap PMD 回放真实 UDP 帧，走完整的 DPDK 收包路径
# （rx_burst → 解析 → 判定 → 统计），不需要 root / 大页 / 物理网卡，也不碰板卡。
#
# 覆盖：正常递增、跳跃、重复、回退（流重启）、坏 magic、中途接入（首包非 0）、
#       非目标端口（--chans 之外）、通道子集映射。
set -euo pipefail
cd "$(dirname "$0")/.."

PCAP=/tmp/pktloss_test.pcap
OUT=/tmp/pktloss_test.out
CSV=/tmp/pktloss_test.csv

make -s

echo "== 判定逻辑自检 =="
./pktloss --selftest

echo "== 生成回放流量 =="
python3 tests/gen_pcap.py "$PCAP"

echo "== pcap PMD 回放（--chans 0,2,4,6，软分流）=="
rm -f "$CSV"
# -m 2048：pktloss 的 mbuf 池固定 131072 × 9216 B ≈ 1.24 GiB（NB_MBUF/MBUF_DATA_ROOM
# 在 pktloss.c 里），-m 1024 建池必失败。--no-huge 下这是纯匿名内存。
# --no-drain：pktloss 默认在放行 worker 前排空队列（丢掉真机中途接入时 flow
# 生效前的残留包）。pcap PMD 是一次性回放，排空会把待测的帧全扔掉。
./pktloss --no-huge --no-shconf --no-pci -m 2048 -l 0-1 \
    --vdev="net_pcap0,rx_pcap=$PCAP" -- \
    --chans 0,2,4,6 --soft-steer --no-drain -d 2 -i 0.5 --csv "$CSV" -q >"$OUT" 2>&1

echo "== 校验 =="
# ch0 的序列 0,1,2,5,6,6,3,4 在 3 处回退 → 统计清零，只剩第二轮(3,4) + 坏包
python3 tests/check_csv.py "$CSV" \
    0:2,0,0,0,1,0,1 \
    2:10,0,0,0,0,0,0 \
    4:2,9999,1,9999,0,0,0 \
    6:2,0,0,0,0,0,0

grep -q "非目标包 3 个" "$OUT" || {
    echo "FAIL: 非目标包计数不是 3"
    grep "非目标" "$OUT" || true
    exit 1
}
echo "  非目标包计数 ok（ch1 的 3 个包被正确排除）"

grep -q "0xAA55AA55" "$OUT" && echo "  坏头告警已打印"

echo "全部通过（日志: $OUT）"
