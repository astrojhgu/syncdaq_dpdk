# pktloss — syncdaq 100G 数据面丢包率探针（DPDK）

绕过内核，用 DPDK 直接收板卡上行 UDP 载荷（8224 B = 32 B MetaData + 8192 B 采样），
只读 MetaData 里的帧编号 `pkt_cnt`，靠**编号跳跃**判断丢包。不落盘、不做 DSP，
CPU 只碰每个包的前 32 字节。

单进程多路：一个进程、每通道一个 RX 队列、每队列一个独占 lcore（`rte_flow` 按
UDP 目的端口精确分流），主 lcore 只做统计和信号。DPDK 独占网卡，所以必须在一个
程序里同时处理全部通道。

## 1 判据

每个通道（UDP 目的端口 `4000+ch`）内编号应严格 +1 递增：

| 情况 | 判定 | 计数 |
|---|---|---|
| `seq == expected` | 正常 | `recv++` |
| `seq > expected` | 丢 `seq-expected` 个 | `lost += gap`，`gaps++` |
| `seq == last` | 重复包 | `dup++`（不改基线） |
| `seq < expected`、`!= last`、**`seq == 0`** | **新一轮**：StreamStart 把 `pkt_cnt` 清零，这是唯一合法的重启形式 | 该通道本轮统计**全部清零**，`rounds++`，这一包作为新一轮首包 |
| `seq < expected`、`!= last`、**`seq != 0`** | **致命故障**：FPGA 数据面错乱（计数器错乱 / 通道串扰 / 乱序发帧） | 立即停止接收，打印现场（通道、期望编号、实收、时刻），走正常关闭流程后 **`exit(1)`**；汇总里 `abn=1` |

丢包率取**上界** `(1 + lost) / recv`（与 `capture_pipeline.rs` / `pipeline.rs` 里
`ratio < (1 + ndropped) / nreceived` 一致）：分子 +1 覆盖"序列末尾可能没被观测到的
那一个"，分母用实收数而非期望总数。所以 `lost=0` 时结果是 `1/recv` 而不是 0 ——
意思是"在这个样本量下，仍可能有 1 个丢包没被看见"。**只针对当前这一轮**。编号回退到 0 时该通道的
`recv/lost/gaps/maxgap/dup/bad` 全部清零，所以每次 StreamStop→StreamStart 之后
汇总只反映新开的那一轮；`rounds` 累计不清零，用来看总共跑了几轮。
吞吐分母也从最近一次归零算起，不会把清零前的数据混进来。

**不假定起点是 0**：先起接收再 StreamStart（首包 0）、先开流再起接收（首包非 0）、
接收中 Stop→Start（回退到 0）三种情况都按上表处理。

**非零回退直接停机，不警告、不清零、不跳过**：`pkt_cnt` 只在 StreamStart 时清零，
所以"回退到 0"是固件重启唯一会产生的形式，其余回退只可能来自 FPGA 侧。从那一包
开始该通道的编号不再可信，继续收只是在生产垃圾统计——所以 `pktloss` 立刻停止接收，
打印现场，走正常关闭流程后以退出码 1 结束，由外层脚本/监控捕获。

早先的两版实现都不对：第一版把任何回退当新流清零，等于把故障静默吞成"重启"；
第二版告警后继续跑，污染数据照样进统计。**代价是**长稳测试里一次偶发乱序就会中止，
需要外层自动重启；这是有意选择——宁可中断，也不让故障数据混进丢包率。

线上格式已实测确认（2026-10-24，板卡 192.168.5.164，320 MSps）：

```
len=8224  magic=0xAA55AA55@0  ver=0x11223344@4  port_id@8  data_type=16@12
          pkt_cnt u64 LE@16（连续递增 1）  tail=0xAA55AA5544332211@24
源 10.100.11.20..27 → 10.100.11.1:4000..4007
```

## 2 环境（本机已就绪）

| 项 | 状态 |
|---|---|
| 网卡 | `eno1np0` = ConnectX-5 `0000:01:00.0`，100 Gb/s，MTU 9000，`10.100.11.1/24` |
| 控制面 | `eth0 192.168.5.1/24`；板卡当前 `192.168.5.164:3000` |
| DPDK | 23.11.4（`libdpdk-dev`），mlx5 PMD 由 EAL 自动从 `dpdk/pmds-24.0` 加载 |
| 大页 | 1024 × 2M = 2 GiB，挂 `/dev/hugepages` |
| sudo | `/home/user/src/syncdaq_dpdk/pktloss` 已配 NOPASSWD（`run.sh` 直接 `sudo`） |
| 负载 | 8 路 × 320 MSps = **156.25 kpps/通道、1.25 Mpps、82.4 Gbps** |

mlx5 走 bifurcated driver：**不需要** `dpdk-devbind.py` unbind，内核接口保留；
但**程序运行期间该口的数据面被 DPDK 独占**，内核侧收不到包，退出后自动恢复
（已实测：退出后 `/sys/class/net/eno1np0/statistics/rx_packets` 继续每秒 +1.24 M）。

## 3 编译 + 免 root 自测

```bash
cd ~/src/syncdaq_dpdk
make
./pktloss --selftest            # 判定逻辑（7 个合成场景，不需要 DPDK）
./tests/run_pcap_test.sh       # 端到端：pcap PMD 回放真实 UDP 帧，不需要 root
```

`run_pcap_test.sh` 验证：正常递增、跳跃 2、大跳跃 9999、重复、回退到 0（合法重启）、
坏 magic、中途接入（首包 100）、非目标端口排除、`--chans 0,2,4,6,7` 子集映射，
以及**非零回退必须停机且退出码非 0**。全绿才动实机。

## 4 实机测量流程（顺序很重要）

板卡地址会变，先探活并确认状态：

```bash
cd ~/src/syncdaq
./target/release/bcast_cmd -a 192.168.5.255:3000 -c cmd/Query.yaml -d 1 -t 3
./target/release/send_cmd  -a 192.168.5.164:3000 -c cmd/Query.yaml -d 0 -t 2   # 看 trans_state / pkt_cnt1
```

标准一轮：

```bash
# 1) 停流并等 PPS 生效（手册要求 ≥1 s，留 4 s 余量）
cd ~/src/syncdaq_dpdk
BOARD=192.168.5.164 ./tools/send.sh ~/src/syncdaq/cmd/StreamStop.yaml
sleep 4

# 2) 起 DPDK 接收（终端 A），等它打印 "端口 0: link up" 再继续
sudo ./run.sh --chans 0-7 -d 30

# 3) 接收端就绪后开流（终端 B，PPS 对齐）
BOARD=192.168.5.164 ./tools/send.sh ~/src/syncdaq/cmd/StreamStart.yaml

# 4) 结束后停流（终端 B）
BOARD=192.168.5.164 ./tools/send.sh ~/src/syncdaq/cmd/StreamStop.yaml
```

**必须先起接收、再 StreamStart**。这样首包 `pkt_cnt == 0`，且没有启动瞬态（见第 5 节）。
反过来（板卡已在传、程序中途接入）也能测，但头几秒会有一段假丢包。

## 5 实测基线（2026-10-24，8 路满速）

| 场景 | 收包 | lost | 丢包率上界 | imissed | 吞吐 |
|---|---|---|---|---|---|
| 8 路，先起接收，12 s | 14 964 669 | 2 | 1.3e-7 | 0 | 81.55 Gbps / 99.2% |
| 8 路，先起接收，15 s | 18 816 489 | 4 | 2.1e-7 | 0 | 81.5 Gbps |
| 8 路，中途接入，20 s | 24 814 965 | 41 416 | 1.7e-3 | 135 234 | 81.57 Gbps |
| 8 路，两轮（第一轮 12 s + 第二轮 12 s） | 15 087 123（只剩第二轮） | 4 | 2.65e-7 | 0 | 81.55 Gbps / 99.2% |
| 8 路，**换新光模块（SR4）后 90 s** | 108 145 807 | **0** | **0** | 0 | 81.5 Gbps |
| 8 路，**新模块 180 s** | 219 705 992 | **0** | **0** | 0 | 81.55 Gbps |
| 4 路（板卡侧关 4–7），先起接收，20 s | 10 801 847 | **0** | **0** | **0** | 41.8 Gbps（满速） |
| 只收 4 路但板卡仍发 8 路，20 s | 12 348 454 | 103 160 | 8.3e-3 | 142 475 | 40.6 Gbps |

结论：

1. **链路和板卡是干净的**：`imissed=0` 时 `lost` 只有 1–4 个包（27 M 包里 4 个，2e-7）。
2. **中途接入有启动瞬态**：头 ~5 秒丢 41 k（其中 ch0 一次 41 383 ≈ 0.26 s），之后 15 秒
   只丢 6 个包（4e-7）。原因是程序介入时板卡已在满速发，`rte_eth_dev_start` 到 worker
   真正开始收之间队列被填满 + CPU 升频。**先起接收再开流即可完全避开**。
3. **8 路 82 Gbps 是主机侧的极限附近**，但稳态零丢包（`imissed=0`）。
   两轮测试里第一轮跑了 12 s、第二轮跑 12 s，汇总只剩第二轮的 15.09 M 包
   （第一轮 ~14 M 包被归零清掉），`轮次=1`、`编号范围 首 0`。
4. **只收子集不能降负荷**：未列入 `--chans` 的通道流量会走网卡默认 RSS 灌进你的队列
   （实测非目标包 12.3 M，`lost` 反而从 60 k 涨到 103 k），PCIe 带宽和 CPU 一点没省。
   要降负荷必须在**板卡侧关流**。

## 6 输出解读

每秒一块，每通道一行：

```
   t(s)  ch       recv     pkt_cnt       pps      lost    dlost    gaps  maxgap      rate_ub  rnd  dup  abn  bad free%
   1.00   0     156250      156249    156250         0        0       0       0    1.000e-01    0    0    0    0  50.4
```

每块的表头行前面还有一行 `--- t = ... mbuf_avail=... max_pkt_cnt=...`，
`max_pkt_cnt` 是打印时刻**所有通道里最大的** `pkt_cnt`（还没收到包时是 `-`）。

| 列 | 含义 |
|---|---|
| `recv` | 收到且头部合法的包 |
| `pkt_cnt` | 本通道最新一包的帧编号（payload off 16 的 `u64`，StreamStart 清零）。取的是 worker 每包写下的 `last_seq`，主线程只在打印时读一遍，热路径零开销；可能比真实值差几个正在 `rx_burst` 里还没处理的包 |
| `pps` | 本区间瞬时包率（理论 156 250/s/通道） |
| `lost` | 累计跳跃量 = 估计丢包数 |
| `dlost` | 本区间新增丢包（定位突发） |
| `gaps` / `maxgap` | 跳跃段数 / 单次最大跳跃 |
| `rate_ub` | 丢包率上界 `(1+lost)/recv`；`lost=0` 时是 `1/recv` |
| `rnd` / `dup` | 轮次（编号**回退到 0** 的次数）/ 重复包 |
| `abn` | 非零回退次数。命中即停机，所以只会是 0 或 1；非 0 = 本次运行因 FPGA 故障中止 |
| `bad` | 头部 magic 或长度不符 |
| `free%` | mbuf 池剩余；接近 0 说明主机侧要丢了 |

结束时的**归因**段是判读核心：

```
帧编号跳跃总数（估计丢包）: 1234
主机侧丢包（imissed + rx_nombuf）: 1200
判定: 跳跃基本可由主机接收侧解释 —— 瓶颈在本机网卡/mbuf/队列
```

- `imissed + rx_nombuf ≈ lost` → 丢在主机接收路径（mbuf 不足、队列满、PCIe/内存带宽）；
- `lost >> imissed` → 残余跳跃在主机之外（板卡停发、链路/PFC），**这才可能是板卡侧**；
- `imissed` 是端口级累计量，跨通道求和后与总跳跃比较；某通道跳跃而 `imissed` 不涨，
  优先怀疑该通道链路/发送端。

程序自己有没有丢包，看三处：`free%` 是否见底、`imissed` 是否为 0、
`hw 计数` 里的 `rx_out_of_buffer` / `rx_steer_missed_packets`。

吞吐用"从首个有效包到结束"的时长算，所以先起接收、等几十秒再开流也不会把吞吐算低
（汇总里会注明"有效接收 X s / 首个有效包在 Y s 到达"）。

## 7 参数

```
sudo ./run.sh [选项]        # EAL 参数由 run.sh 拼；也可直接 sudo ./pktloss <EAL> -- <选项>

  -p, --pci ADDR        DPDK 设备（run.sh 用 PCI 环境变量，默认 0000:01:00.0）
  -b, --base-port N     UDP 目的端口基址（默认 4000）
      --chans LIST      要收的通道（默认 0-7）：0-3 / 0,2,4,6 / 0,2-3
  -d, --duration SEC    运行秒数（0 = 直到 Ctrl-C）
  -r, --sps HZ          采样率，用于吞吐百分比（默认 320e6）
  -i, --interval SEC    打印间隔（默认 1.0）
  -c, --csv PATH        追加 CSV（unix_time,elapsed,chan,recv,pkt_cnt,pps,lost,gaps,
                        maxgap,rate_ub,rounds,dup,abn,bad,mbuf_avail）
  -s, --soft-steer      单队列 + 软件分流（调试/pcap 用；性能测试别开）
  -q, --quiet           不打印无数据的通道行
      --no-drain        跳过启动窗口排空（pcap 回放等一次性流量必须开，
                        否则待测帧会被当成 flow 生效前的残留全丢掉）
      --sched-fifo N    worker 用 SCHED_FIFO N（0 = 不设，防 CPU 抢占丢包）
      --dump N          打印前 N 个包的 mbuf 原始头部（排障用）
      --selftest        只跑判定逻辑自检
  -h, --help            帮助（必须在 `--` 之前也能被截获）
```

`run.sh` 环境变量：`PCI`、`LCORES`（默认 `0-8`，主核 + 8 个收包核）、`SOCKET_MEM`（MB）、
`RT_PRIO`（worker 的 SCHED_FIFO 优先级，默认 50，0 = 关）。

队列模型：默认每个通道一个 RX 队列，一条 `rte_flow` 把 `UDP dport=4000+ch` 钉到队列 `ch`，
队列 `ch` 由 lcore `ch` 独占 —— 通道内严格保序，"回退"只可能是新流。`--soft-steer` 退化为
单队列单核，仅用于调试。

## 8 坑

1. **地址漂移**：`~/src/syncdaq/aligned_send.sh` 写死 `192.168.5.174`，当前板卡是
   `192.168.5.164`。用 `tools/send.sh`（`BOARD=` 可配）。
2. **`cmd/XGbeCfgSingle4.yaml` 只写 port 0–3**，port 4–7 保留上次配置；之前 8 路全开时
   发它会变成"以为关了 4 路、实际 8 路还在发"。要关必须用 `_4chan.yaml` /
   `XGbeCfgSingle_one.yaml` 这类把 8 条写全、不需要的 `dst_mac` 置零的文件
   （固件按 `dst_mac != 0` 决定是否发包）。
3. **快速反复 Stop/Start 会把数据路径冻结**（`pkt_cnt` 一直涨但收不到数据），恢复需
   重新 `init_rfsoc` → `Sync`。每轮之间隔 ≥3 s。
4. **`StreamStop` 后至少等 2 s 再数包**，否则数到的是生效前的尾巴。
5. **DPDK 占用期间内核工具数到 0 是正常的**：`recv_perf`、`/sys/class/net/eno1np0/statistics`
   都不再反映数据面。
6. **集中丢包（某一刻一次丢一大批）与随机单包丢包是两回事**。RX 队列 4096 深、
   每核 155 kpps ≈ **26 ms 缓冲**：CPU 时间片轮转（几 ms 让出）填不满队列，
   只有**连续 >26 ms 的停顿**才会丢一批——RT 进程抢占、内核长路径、内存回收、
   SMI。判读方法：看每秒 port 行的 `out_of_buffer` 增量（>0 = 主机队列溢出），
   再看表格的 `dlost` / `maxgap`（一次跳几千且与 `out_of_buffer` 同步 = 队列溢出；
   分散的单包跳 = 链路误码）。
   **实测数据**（`chrt -f 60` 抢占 CPU 6 十秒）：
   - worker `SCHED_OTHER`：每秒丢 143,128 个包（`out_of_buffer` 同步上涨），
     `143k ≈ 0.92 s × 155 kpps`，因为 RT throttling 每秒只给非 RT 任务 50 ms；
   - worker `SCHED_FIFO 70`：被抢占的情况消失了，但 **RT throttling 作用于 worker
     自己**，每秒被强制停 ~24 ms，每通道每秒丢 3,720 个（`maxgap 3,743 ≈ 队列深度`）。
     所以 `run.sh` 默认 **`RT_PRIO=0`**，不用 RT。

   **throttling 关闭后的对照（`chrt -f 60` 抢 CPU 6 十秒）**：
   - worker `SCHED_FIFO 70`：抢占期间 **ch5 零丢包、`out_of_buffer=0`**；
   - worker `SCHED_OTHER`：一次 **1,544,343** 的大 gap（`out_of_buffer` 同步 1.54 M），
     因为 RT60 独占十秒、worker 完全没机会跑。

   所以 RT 的正确用法是**两个一起**：
   ```bash
   sudo sysctl -w kernel.sched_rt_runtime_us=-1   # 关 throttling
   sudo RT_PRIO=70 ./run.sh --chans 0-7 -d 60     # worker 用 RT 70
   ```
   只开 RT 不开 throttling 会更糟（每秒 3,720 丢包）。不想用 RT 就保持默认
   `RT_PRIO=0`，用 `tune.sh` 挪中断 + 内核参数
   `isolcpus=1-8 nohz_full=1-8 rcu_nocbs=1-8`（改 GRUB，重启生效）。
7. **程序被 kill -9 会留下大页文件**（`/dev/hugepages/pktlossmap_*`，最多占满 free）。
   `run.sh` 默认带 `--huge-unlink`，EAL 复用后立即 unlink，退出即释放；被 kill -9 的
   残留也会在下次启动时被回收。极端情况下手动清：`sudo rm -f /dev/hugepages/pktlossmap_*`。
7. **`--help` 必须在 EAL 参数之前被截获**，否则打印的是 DPDK 的参数列表（已处理）。

## 9 链路丢包的排查方法（实测流程）

按层次对账，一层层排除：

| 层 | 看什么 | 命令/输出 |
|---|---|---|
| 板卡打包 | `pkt_cnt1` 增量、`fifo_full_cnt` | `Query.yaml` |
| 链路 PHY | `rx_phy_packets`、`rx_crc_errors_phy`、`rx_symbol_err_phy`、`rx_pause_ctrl_phy` | `sudo ethtool -S eno1np0` |
| 网卡队列 | `out_of_buffer`、`imissed` | pktloss 每秒 `port:` 行 |
| 应用层 | `lost`、`gaps`/`maxgap`、`abn` | pktloss 表格与汇总 |

判据：

- `rx_crc_errors_phy` 增量 ≈ `lost` → **链路误码**（FEC 纠不过来的残留），查光模块/光纤；
- `out_of_buffer`/`imissed` 增量 > 0 → **主机队列溢出**（CPU 抢占/带宽），加 `--sched-fifo`、
  迁中断、或 isolcpus；
- 两者都不涨但 `lost` 涨 → 板卡 `pkt_cnt` 跳号（PL/CMAC 侧）。

2026-10-24 的实测结论：旧 LR4 模块稳态 `lost/recv ≈ 1.9e-7`，与 `rx_crc_errors_phy`
增量 1:1；换 SR4 模块后 180 s / 2.2 亿包 **`lost=0`、CRC 增量 0**，问题在旧模块/光路。

## 10 大页配置（需要临时加页时）

当前 1024 × 2M = 2 GiB，`--socket-mem 1536` 用 768 页，够用。要加：

```bash
# 立即生效（重启后失效）
sudo dpdk-hugepages.py -p 2M -r 4G
# 等价写法
echo 2048 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# 永久生效
echo 'vm.nr_hugepages = 2048' | sudo tee /etc/sysctl.d/99-hugepages.conf
sudo sysctl --system

# 1 GiB 页（必须内核启动参数，重启生效）
# /etc/default/grub: GRUB_CMDLINE_LINUX_DEFAULT="... default_hugepagesz=1G hugepagesz=1G hugepages=4"
sudo update-grub && sudo reboot
```

mbuf 池按 `NB_MBUF(65536) × (MBUF_DATA_ROOM(9216) + ~256)` ≈ 600 MiB 估算；加大池或
`--socket-mem` 时同步加页。

## 11 实测采样率：用 1 PPS 数采样点

板卡只开 ch0、ADC 接 1 PPS（功分器 AC 耦合后是窄微分脉冲），记录时域降采样再找脉冲间隔：

```bash
# 板卡侧：只开 ch0
send_cmd -a 192.168.5.164:3000 -c ~/src/syncdaq/cmd/XGbeCfgSingle_only_port0.yaml -d 0 -t 3
# 采集 4 秒时域（每 4 个采样点存 1 个，写内存）
sudo ./pktloss -l 0-1 -a 0000:01:00.0 --socket-mem 1536 --      --chans 0 --raw-ch 0 --raw-stride 4 --raw-file /dev/shm/raw.bin -d 4
```

2026-10-24 实测（PPS 输入 ch0）：

| 间隔 | 采样点数 | 折算采样率 |
|---|---|---|
| 1 个 PPS 周期 | 317,307,712 | **317.3077 MSps** |
| 2 个 PPS 周期 | 634,615,456 / 2 | **317.3077 MSps** |

两次独立测量一致到 2.5e-8。**板卡实际采样率 317.3077 MSps，标称 320，偏差 -0.841%**。
这正好解释 `--sps 320e6` 时吞吐只有 99.16% —— 不是丢包，是时钟。

想让百分比显示 100%，用 `--sps 317307700`；想根治要查 LMK04828 输出 / RFDC PLL 分频。
理论包率 = 317.3077e6 / 2048 = **154,935.4 pps/通道**，与实测 154,935–154,938 吻合。
