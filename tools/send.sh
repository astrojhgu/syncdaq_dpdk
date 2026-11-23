#!/usr/bin/env bash
# 半秒对齐发板卡指令 —— StreamStart / StreamStop 这类 PPS 对齐的指令必须这样发。
#
#   BOARD=192.168.5.164 ./tools/send.sh ~/src/syncdaq/cmd/StreamStop.yaml
#   BOARD=192.168.5.164 ./tools/send.sh ~/src/syncdaq/cmd/StreamStart.yaml
#
# 不用 ~/src/syncdaq/aligned_send.sh 的原因：它把板卡地址写死成 192.168.5.174，
# 而地址来自 DHCP 池（当前是 192.168.5.164）。先用广播探活确认：
#   cd ~/src/syncdaq && ./target/release/bcast_cmd \
#       -a 192.168.5.255:3000 -c cmd/Query.yaml -d 1 -t 3
set -euo pipefail

BOARD=${BOARD:-192.168.5.164}
SEND=${SEND:-$HOME/src/syncdaq/target/release/send_cmd}
CMD=${1:?用法: BOARD=<ip> $0 <cmd.yaml> [timeout]}
TIMEOUT=${2:-2}

[ -x "$SEND" ] || {
    echo "找不到 $SEND（先 cd ~/src/syncdaq && cargo build --release）" >&2
    exit 1
}

now=$(date +%s.%N)
target=$(awk -v n="$now" 'BEGIN{t=int(n-0.5)+1.5; printf "%.6f", t}')
d=$(awk -v t="$target" -v n="$now" 'BEGIN{x=t-n; if(x<0)x=0; printf "%.6f", x}')
if awk -v d="$d" 'BEGIN{exit !(d>0.002)}'; then
    sleep "$d"
fi

exec "$SEND" -a "$BOARD:3000" -c "$CMD" -d 0 -t "$TIMEOUT"
