#!/bin/bash
LOG=/tmp/pps_recheck3.log
: > "$LOG"
CMD=/home/user/src/syncdaq/target/release/send_cmd
CFG=/home/user/src/syncdaq/cmd
DPDK=/home/user/src/syncdaq_dpdk
BOARD=192.168.5.164

wait_app() {
  for i in $(seq 1 25); do
    if timeout 12 "$CMD" -a "$BOARD":3000 -c "$CFG/Query.yaml" -d 0 -t 8 >/dev/null 2>&1; then
      echo "APP_READY after $((i*3))s" >> "$LOG"; return 0
    fi
    sleep 3
  done
  echo "APP_TIMEOUT" >> "$LOG"; return 1
}

run_one() {
  local key="$1" proj="$2" mixer="$3"
  echo "########## $key : $proj ##########" >> "$LOG"
  cd ~/fpga/"$proj" || return 1
  timeout 400 scripts/jtag_run.sh >> "$LOG" 2>&1
  echo "JTAG $key rc=$?" >> "$LOG"
  cd "$DPDK" || return 1                      # 关键：切回 DPDK 目录
  wait_app || return 1
  local ok=0
  for t in 1 2 3; do
    if timeout 30 "$CMD" -a "$BOARD":3000 -c "$CFG/ext_clk.yaml" -d 0 -t 20 >> "$LOG" 2>&1; then
      echo "EXTCLK $key OK (try $t)" >> "$LOG"; ok=1; break
    fi
    echo "EXTCLK $key fail try $t" >> "$LOG"; sleep 3
  done
  [ $ok -eq 1 ] || echo "EXTCLK $key ALL_FAIL" >> "$LOG"
  sleep 3
  timeout 45 "$CMD" -a "$BOARD":3000 -c "$CFG/XGbeCfgSingle_only_port0.yaml" -d 0 -t 30 >> "$LOG" 2>&1
  timeout 15 "$CMD" -a "$BOARD":3000 -c "$DPDK/$mixer" -d 0 -t 8 >> "$LOG" 2>&1
  timeout 15 "$CMD" -a "$BOARD":3000 -c "$DPDK/dsa_0.yaml" -d 0 -t 8 >> "$LOG" 2>&1
  "$DPDK/tools/send.sh" "$CFG/StreamStart.yaml" 5 >> "$LOG" 2>&1
  sleep 3
  timeout 15 "$CMD" -a "$BOARD":3000 -c "$CFG/Query.yaml" -d 0 -t 8 2>&1 | grep -E "trans_state|smp_rate|pkt_cnt1" >> "$LOG"
  rm -f /dev/shm/pps3_$key.dat
  sudo -n "$DPDK/pktloss" -l 0-1 -n 4 -a 0000:01:00.0 --socket-mem 1536 --file-prefix pktloss \
      --huge-unlink --log-level=lib.eal:warning -- --chans 0 --raw-ch 0 --raw-stride 1 \
      --raw-file /dev/shm/pps3_$key.dat --raw-no-seq -d 2 -i 2 >> "$LOG" 2>&1
  echo "CAPTURE $key rc=$? size=$(stat -c %s /dev/shm/pps3_$key.dat 2>/dev/null)" >> "$LOG"
}

run_one 320     sync_daq_320MSps_iq      mixer_nco257.yaml
run_one 100     sync_daq_100MSps_iq      mixer_nco257.yaml
run_one 100_8b  sync_daq_100MSps_iq_8bit mixer_nco257.yaml
run_one 640_8b  sync_daq_640MSps_iq      mixer_nco400.yaml
echo "ALL_DONE" >> "$LOG"
