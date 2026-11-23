#!/usr/bin/env bash
# 无特权端到端自测：用 pcap PMD 回放真实 UDP 帧，走完整的 DPDK 收包路径
# （rx_burst → 解析 → 判定 → 统计），不需要 root / 大页 / 物理网卡，也不碰板卡。
#
# 覆盖：正常递增、跳跃、重复、回退到 0（合法重启）、坏 magic、中途接入（首包非 0）、
#       非目标端口（--chans 之外）、通道子集映射，以及非零回退（FPGA 故障）必须停机。
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

echo "== pcap PMD 回放（--chans 0,2,4,6,7，软分流）=="
rm -f "$CSV"
# -m 2048：pktloss 的 mbuf 池固定 131072 × 9216 B ≈ 1.24 GiB（NB_MBUF/MBUF_DATA_ROOM
# 在 pktloss.c 里），-m 1024 建池必失败。--no-huge 下这是纯匿名内存。
# --no-drain：pktloss 默认在放行 worker 前排空队列（丢掉真机中途接入时 flow
# 生效前的残留包）。pcap PMD 是一次性回放，排空会把待测的帧全扔掉。
./pktloss --no-huge --no-shconf --no-pci -m 2048 -l 0-1 \
    --vdev="net_pcap0,rx_pcap=$PCAP" -- \
    --chans 0,2,4,6,7 --soft-steer --no-drain -d 2 -i 0.5 --csv "$CSV" -q >"$OUT" 2>&1

echo "== 校验 =="
# ch0 0,1,2,5,6,6：5 跳跃 2；6 重复。坏 magic 那个包只计 bad（不进编号序列）。
#   → recv=5 lost=2 gaps=1 rnd=0 dup=1 abn=0 bad=1
# ch7 0,1,2,0,1,2：第二个 0 是合法重启 → 清零重开，rnd=1，末态 recv=3
python3 tests/check_csv.py "$CSV" \
    0:5,2,1,2,0,1,0,1 \
    2:10,0,0,0,0,0,0,0 \
    4:2,9999,1,9999,0,0,0,0 \
    6:2,0,0,0,0,0,0,0 \
    7:3,0,0,0,1,0,0,0

grep -q "非目标包 3 个" "$OUT" || {
    echo "FAIL: 非目标包计数不是 3"
    grep "非目标" "$OUT" || true
    exit 1
}
echo "  非目标包计数 ok（ch1 的 3 个包被正确排除）"

grep -q "0xAA55AA55" "$OUT" && echo "  坏头告警已打印"

echo "== 非零回退 = FPGA 故障：必须停机并以非 0 退出 =="
PCAP_F=/tmp/pktloss_fault.pcap
OUT_F=/tmp/pktloss_fault.out
python3 tests/gen_pcap.py "$PCAP_F" --fault
set +e
./pktloss --no-huge --no-shconf --no-pci -m 2048 -l 0-1 \
    --vdev="net_pcap0,rx_pcap=$PCAP_F" -- \
    --chans 0 --soft-steer --no-drain -d 3 -i 0.5 -q >"$OUT_F" 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || {
    echo "FAIL: 非零回退后退出码是 0（应当非 0）"
    tail -20 "$OUT_F"
    exit 1
}
grep -q "FPGA 数据面故障，停止接收" "$OUT_F" || {
    echo "FAIL: 没有 FPGA 故障提示"
    tail -20 "$OUT_F"
    exit 1
}
grep -q "退出码 1" "$OUT_F" || {
    echo "FAIL: 汇总里没有故障说明"
    tail -20 "$OUT_F"
    exit 1
}
echo "  非零回退 → 退出码 $rc，已停机并报故障"

echo "全部通过（日志: $OUT）"
