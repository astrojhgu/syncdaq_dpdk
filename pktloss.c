/* pktloss.c — syncdaq 100G 数据面的丢包率探针（DPDK / ConnectX-5）
 *
 * 做什么：绕过内核直接收板卡上行的 UDP 载荷（8224 B = 32 B MetaData +
 * 8192 B 采样），只读 MetaData 里的帧编号 pkt_cnt，靠编号跳跃判断丢包。
 * 不落盘、不做任何 DSP，CPU 只碰每个包的头部（32 B），采样数据全靠网卡 DMA
 * 写进 mbuf 然后立即释放。
 *
 * 线上格式（与 doc/manual.md、doc/system_notes.typ "UDP 载荷" 一节、以及
 * syncdaq/src/payload.rs 的 Payload 结构一致，小端）：
 *   off  0  u32  head_magic   = 0xAA55AA55
 *   off  4  u32  version      = 0x11223344
 *   off  8  u32  port_id      = 通道号（= UDP 目的端口 - 4000）
 *   off 12  u32  data_type    = 16（i16 固件）或 8
 *   off 16  u64  pkt_cnt      = 通道内包计数，StreamStart 时清零
 *   off 24  u64  tail_magic   = 0xAA55AA5544332211
 *   off 32  ...  8192 B 采样
 *
 * 判据（与 test 需求一致）：
 *   seq == expected                 → 正常
 *   seq >  expected                 → 丢 seq-expected 个，累加 lost
 *   seq == last_seq                 → 重复包，计 dup，不计丢包
 *   seq <  expected 且 != last_seq  → 编号回退 = 新一轮数据流（StreamStart 清零
 *                                     是最常见的形式）。该通道的统计整体清零：
 *                                     recv/lost/gaps/maxgap/dup/bad 全归零，
 *                                     rounds 累计 +1，这一包作为新一轮的首包。
 *   丢包率取**上界** (1 + lost) / recv（同 capture 的估算），只针对当前这一轮。
 *
 * 归因：帧编号跳跃有两种来源——链路/发送端真丢，或主机侧网卡/mbuf 不足丢包。
 * 后者由 rte_eth_stats 的 imissed / rx_nombuf 和 mlx5 的 rx_out_of_buffer 反映。
 * 汇总里同时给两者，并算"扣除主机侧丢包后的残余跳跃"。
 *
 * 队列模型：
 *   默认（硬 steer）：每个通道一个 RX 队列，一条 rte_flow 规则把 UDP dst_port
 *   = base+ch 的包钉到队列 ch，队列 ch 由 lcore ch 独占，单线程顺序处理，
 *   所以通道内严格保序，"回退"只可能是新流。
 *   --soft-steer：单队列收全部，软件按 dst_port 分流（网卡不支持 dport flow、
 *   或 pcap PMD 自测时用）。
 *
 * 构建：make
 * 运行：sudo ./run.sh ...（见 README.md）
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_version.h>

/* ------------------------------------------------------------------ 常量 */

#define MAX_CHAN        64      /* 通道数上限（实际 8） */
#define BURST           32      /* rx_burst 批大小 */
#define RX_DESC         8192    /* 每队列描述符数 */
#define NB_MBUF         131072  /* mbuf 池总大小（8 队列 × 8192 desc + 等量空闲余量） */
#define MBUF_CACHE      256
#define MBUF_DATA_ROOM  9216    /* 9000(MTU) + 128(headroom) + 余量 */
#define LINK_MTU        9000

#define META_LEN        32
#define SAMPLE_LEN      8192
#define FRAME_LEN       (META_LEN + SAMPLE_LEN)  /* 8224，UDP 载荷长度 */

#define OFF_MAGIC       0
#define OFF_VERSION     4
#define OFF_PORTID      8
#define OFF_DATATYPE    12
#define OFF_SEQ         16
#define OFF_TAIL        24

#define HEAD_MAGIC      0xAA55AA55u
#define TAIL_MAGIC      0xAA55AA5544332211ull

/* 采样点占 payload 的比例：8192 B / 4 B per complex-i16 */
#define BYTES_PER_SAMPLE 4u

/* ------------------------------------------------------------------ 统计 */

enum verdict { V_FIRST, V_OK, V_GAP, V_DUP, V_RESTART };

/* 每通道状态。worker 写、主线程读，所以字段用 atomic（relaxed 即可：
 * 单写者、只需要避免撕裂，不需要跨字段顺序）。 */
struct ch_track {
    _Atomic uint64_t recv;      /* 收到的有效包 */
    _Atomic uint64_t lost;      /* 累计跳跃量（= 估计丢包数） */
    _Atomic uint64_t gaps;      /* 跳跃事件次数 */
    _Atomic uint64_t maxgap;    /* 单次最大跳跃 */
    _Atomic uint64_t rounds;    /* 轮次：编号回退/归零的次数，累计不清零 */
    _Atomic uint64_t dup;       /* 重复包 */
    _Atomic uint64_t bad;       /* 头不符（magic/长度） */
    _Atomic uint64_t first_seq;
    _Atomic uint64_t last_seq;
    _Atomic uint64_t expected;  /* 下一个期望编号 */
    _Atomic int      started;
};

static struct ch_track chans[MAX_CHAN];

/* 全局配置与运行状态 */
static uint16_t g_base_port   = 4000;
static int      g_chans[MAX_CHAN];  /* 实际要收的通道号（升序去重） */
static int      g_nchan;            /* = g_chans 的长度 */
static int      g_dport_idx[256];   /* dport-base → g_chans 下标，-1 = 不收 */
static double   g_sps         = 320e6;
static double   g_duration    = 0.0;    /* 0 = 跑到 Ctrl-C */
static double   g_interval    = 1.0;
static int      g_soft_steer  = 0;
static int      g_dump_left  = 0;   /* --dump N：打印前 N 个包的原始头部 */
static int      g_show_xstats = 0;  /* --xstats：打印全部网卡计数器后退出 */
static int      g_raw_chan    = -1;  /* --raw-ch：记录这个通道的时域降采样 */
static int      g_raw_stride  = 32;  /* 每 stride 个采样点存 1 个 */
static FILE    *g_raw_fp;  /* --raw-file：时域降采样输出 */
static int      g_raw_no_seq; /* --raw-no-seq：只写 8192 B 采样，不含 8 B 包序号 */
static int      g_rt_prio     = 0;  /* --sched-fifo N：worker 用 SCHED_FIFO，0 = 不设 */

/* 丢包事件记录：发生时刻 + 编号 + 跳跃量。CPU 抢占造成的集中丢包会表现为
 * "某个时刻一次大 gap"，和链路的随机单包丢失在时间分布上完全不同。 */
#define GAP_EVT_MAX 16
struct gap_evt {
    uint64_t t_ns;
    uint64_t seq;
    uint64_t size;
};
static struct gap_evt g_gevt[MAX_CHAN][GAP_EVT_MAX];
static _Atomic int     g_gevt_n[MAX_CHAN];
static int      g_quiet       = 0;
static int      g_drain_start = 1;  /* --no-drain 关掉：启动窗口排空会吃掉一次性流量 */
static char     g_pci[64];              /* 空 = 取第一个可用端口 */
static char     g_csv_path[512];

static _Atomic int      g_stop;
static _Atomic uint64_t g_other;        /* 非目标端口 / 非 IPv4-UDP / 多段包 */
static _Atomic int      g_warned_bad;
static _Atomic int      g_dump_cnt;
static uint16_t         g_port;           /* 主线程用：端口号 */
static _Atomic int      g_rt_warned;      /* RT throttling 警告只打一次 */
static _Atomic int      g_rx_enabled;

/* rte_exit() 走 exit()，而 worker 线程还在 g_stop/g_rx_enabled 上自旋；EAL 的
 * 清理会等 lcore 停下，于是退出卡死（实测 9 个核满转、768 页大页不释放，只能
 * kill -9）。atexit 是 LIFO，这个比 EAL 的清理后注册、先执行，先把 g_stop 置上，
 * worker 就能自己退出来。 */
static void stop_workers_at_exit(void)
{
    atomic_store_explicit(&g_stop, 1, memory_order_relaxed);
}
static _Atomic uint64_t g_first_pkt_ns;   /* 首个有效包的 CLOCK_MONOTONIC 时刻 */
static _Atomic uint64_t g_last_pkt_ns;    /* 最近一个有效包的 CLOCK_MONOTONIC 时刻 */
static _Atomic uint64_t g_last_reset_ns;  /* 最近一次编号归零的时刻（统计起点） */
static _Atomic uint64_t g_t0_ns;           /* 主循环起点（worker 记录 gap 时刻用） */

/* 端口层计数器（每秒打增量，用来定位丢在哪一层）：
 *   phy_pkts    PHY 收到的包（链路层）
 *   phy_discard PHY 丢弃（CRC/FEC 不可纠错、filter 等）—— 链路丢包的直接证据
 *   oob         mlx5 rx_out_of_buffer（队列无 mbuf）—— 主机侧丢包
 *   imissed     ethdev imissed（同上的上层口径） */
enum { XS_PHY_PKTS, XS_PHY_DISCARD, XS_OOB, XS_IMISSED, XS_N };
static int      g_xidx[XS_N] = { -1, -1, -1, -1 };
static uint64_t g_xprev[XS_N];
static const char *const XS_NAME[XS_N] = {
    "rx_phy_packets", "rx_phy_discard_packets", "rx_out_of_buffer", "imissed"
};  /* 端口 start + flow 就绪后才放 worker 收包 */

static struct rte_mempool *g_pool;
static FILE *g_csv;

/* ------------------------------------------------------------------ 判定 */
/* 纯逻辑，不依赖 DPDK：--selftest 直接调用。返回本次判定，gap_out 给出跳跃量。 */

static enum verdict track_update(struct ch_track *t, uint64_t seq, uint64_t *gap_out)
{
    const uint64_t last    = atomic_load_explicit(&t->last_seq, memory_order_relaxed);
    const uint64_t expect  = atomic_load_explicit(&t->expected, memory_order_relaxed);
    const int      started = atomic_load_explicit(&t->started, memory_order_relaxed);

    if (gap_out)
        *gap_out = 0;

    if (!started) {
        atomic_store(&t->started, 1);
        atomic_store(&t->first_seq, seq);
        atomic_store(&t->last_seq, seq);
        atomic_store(&t->expected, seq + 1);
        atomic_fetch_add(&t->recv, 1);
        return V_FIRST;
    }

    if (seq == last) {                      /* 重复包：不动基线 */
        atomic_fetch_add(&t->dup, 1);
        return V_DUP;
    }

    if (seq == expect) {                    /* 正常递增 */
        atomic_fetch_add(&t->recv, 1);
        atomic_store(&t->last_seq, seq);
        atomic_store(&t->expected, seq + 1);
        return V_OK;
    }

    if (seq > expect) {                     /* 跳跃 = 丢包 */
        const uint64_t gap = seq - expect;

        atomic_fetch_add(&t->recv, 1);
        atomic_fetch_add(&t->lost, gap);
        atomic_fetch_add(&t->gaps, 1);

        uint64_t cur = atomic_load_explicit(&t->maxgap, memory_order_relaxed);
        while (gap > cur &&
               !atomic_compare_exchange_weak_explicit(&t->maxgap, &cur, gap,
                                                     memory_order_relaxed,
                                                     memory_order_relaxed))
            ;

        atomic_store(&t->last_seq, seq);
        atomic_store(&t->expected, seq + 1);
        if (gap_out)
            *gap_out = gap;
        return V_GAP;
    }

    /* seq < expect 且不是重复包 → 编号回退 = 新一轮数据流。StreamStart 会把
     * pkt_cnt 清零，这是最常见的形式。
     *
     * 这里把该通道的统计清零，让 recv/lost/gaps/dup/bad 只反映"最近一轮"：
     * 每次重新开流后，丢包率从这一轮重新算起，而不是跨轮把旧数据混进来。
     * rounds 累计不清零，用来知道到底跑了几轮。 */
    atomic_fetch_add(&t->rounds, 1);
    atomic_store(&t->recv, 0);
    atomic_store(&t->lost, 0);
    atomic_store(&t->gaps, 0);
    atomic_store(&t->maxgap, 0);
    atomic_store(&t->dup, 0);
    atomic_store(&t->bad, 0);
    atomic_store(&t->first_seq, seq);
    atomic_store(&t->last_seq, seq);
    atomic_store(&t->expected, seq + 1);
    atomic_store(&t->recv, 1);
    return V_RESTART;
}

/* ------------------------------------------------------------------ 解析 */

struct pkt_info {
    const uint8_t *payload;   /* 指向 8192 B 采样 */
    uint16_t dport;
    uint32_t port_id;
    uint32_t datatype;
    uint32_t magic;
    uint64_t seq;
    uint32_t plen;
    bool     hdr_ok;
};

static inline uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* 返回 false = 不是我们要的 IPv4/UDP 包（计入 other）。 */
static bool parse_frame(const uint8_t *p, uint32_t len, struct pkt_info *pi)
{
    if (len < 14 + 20 + 8)
        return false;
    if (ntohs(rd16(p + 12)) != 0x0800)      /* EtherType: IPv4 */
        return false;

    const uint8_t  vihl = p[14];
    const uint32_t ihl  = (uint32_t)(vihl & 0x0f) * 4u;

    if ((vihl >> 4) != 4 || ihl < 20 || len < 14 + ihl + 8)
        return false;
    if (p[14 + 9] != 17)                    /* 只处理 UDP */
        return false;
    if (ntohs(rd16(p + 14 + 6)) & 0x3fff)   /* 不做 IP 分片重组（注意是网络序） */
        return false;

    const uint8_t *udp   = p + 14 + ihl;
    const uint16_t ulen  = ntohs(rd16(udp + 4));
    const uint16_t dport = ntohs(rd16(udp + 2));

    if (ulen < 8)
        return false;

    const uint32_t plen = (uint32_t)ulen - 8u;
    if (plen < META_LEN + 8)
        return false;

    const uint8_t *pl = udp + 8;

    pi->payload  = pl;
    pi->dport    = dport;
    pi->plen     = plen;
    pi->magic    = rd32(pl + OFF_MAGIC);
    pi->port_id  = rd32(pl + OFF_PORTID);
    pi->datatype = rd32(pl + OFF_DATATYPE);
    pi->seq      = rd64(pl + OFF_SEQ);
    pi->hdr_ok   = (pi->magic == HEAD_MAGIC) &&
                   (rd64(pl + OFF_TAIL) == TAIL_MAGIC) &&
                   (plen == FRAME_LEN);
    return true;
}

/* ------------------------------------------------------------------ 收包 */

static uint64_t now_mono_ns(void);   /* 前置声明：worker 里给首包打点用 */

/* Linux 的 RT throttling：RT 任务每个 period（默认 1 s）最多运行
 * sched_rt_runtime_us（默认 950 ms），剩下的时间强制让给 SCHED_OTHER。
 * DPDK worker 是空转轮询，同样计入 RT 时间 —— 于是每秒被停 50 ms，
 * 4096 深的队列（约 26 ms）必然溢出，表现为"每秒一次、每次几千个包"的
 * 周期性丢包。实测：worker RT70 时每通道每秒丢 3,720 个。 */
static long read_rt_runtime_us(void)
{
    FILE *f = fopen("/proc/sys/kernel/sched_rt_runtime_us", "r");
    long  v = -1;

    if (f != NULL) {
        if (fscanf(f, "%ld", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

/* 把 worker 提到 SCHED_FIFO：否则系统里任何 SCHED_OTHER 进程/内核线程落在
 * 这个核上都能抢占几毫秒，RX 队列（4096 深 ≈ 26 ms）瞬间溢出，丢掉一批包。 */
static void promote_to_rt(void)
{
    struct sched_param sp;

    if (g_rt_prio <= 0)
        return;

    const long rt_run = read_rt_runtime_us();

    if (rt_run > 0 && atomic_exchange(&g_rt_warned, 1) == 0) {
        fprintf(stderr,
                "警告: kernel.sched_rt_runtime_us=%ld（RT throttling 开启）。\n"
                "      SCHED_FIFO 的 worker 每秒会被强制停 %ld ms，队列填满会周期性丢包，\n"
                "      实测每通道每秒丢 ~3700 个。二选一：\n"
                "        a) sudo sysctl -w kernel.sched_rt_runtime_us=-1   （再跑 RT）\n"
                "        b) 用 --sched-fifo 0（默认），改用 isolcpus/tune.sh 隔离\n",
                rt_run, (1000000 - rt_run) / 1000);
    }

    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = g_rt_prio;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        fprintf(stderr, "警告: 设置 SCHED_FIFO %d 失败（%s），worker 仍可能被抢占\n",
                g_rt_prio, strerror(errno));
}

struct worker_arg {
    uint16_t port;
    uint16_t queue;
    int      soft;
    uint64_t other;     /* 本 worker 的本地计数，退出时汇总 */
};

static int worker_main(void *arg)
{
    struct worker_arg *wa = arg;
    struct rte_mbuf *bufs[BURST];

    /* 先等端口 start 完、flow 建好再碰队列：否则 start 到首个 burst 之间
     * 堆积的包会白白丢掉（表现为开机头几秒的假丢包）。 */
    while (!atomic_load_explicit(&g_rx_enabled, memory_order_acquire)) {
        if (atomic_load_explicit(&g_stop, memory_order_relaxed))
            return 0;
        rte_pause();
    }

    promote_to_rt();

    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        const uint16_t n = rte_eth_rx_burst(wa->port, wa->queue, bufs, BURST);
        if (n == 0) {
            rte_pause();
            continue;
        }

        for (uint16_t i = 0; i < n; i++) {
            struct rte_mbuf *m = bufs[i];
            const uint8_t   *p = rte_pktmbuf_mtod(m, const uint8_t *);

            if (atomic_load_explicit(&g_dump_cnt, memory_order_relaxed) < g_dump_left) {
                const int k = atomic_fetch_add(&g_dump_cnt, 1);

                fprintf(stderr,
                        "[dump %d] nb_segs=%u data_len=%u pkt_len=%u data_off=%u "
                        "ethertype=%04x ipver=%02x proto=%02x head=",
                        k, m->nb_segs, rte_pktmbuf_data_len(m),
                        rte_pktmbuf_pkt_len(m), m->data_off, rd16(p + 12),
                        rte_pktmbuf_data_len(m) > 14 ? p[14] : 0,
                        rte_pktmbuf_data_len(m) > 23 ? p[23] : 0);
                for (uint32_t b = 0; b < 64 && b < rte_pktmbuf_data_len(m); b++)
                    fprintf(stderr, "%02x", p[b]);
                fprintf(stderr, "\n");
            }

            if (unlikely(m->nb_segs != 1)) {
                wa->other++;
                continue;
            }

            struct pkt_info pi;
            if (!parse_frame(p, rte_pktmbuf_data_len(m), &pi)) {
                wa->other++;
                continue;
            }

            const int ch = (int)pi.dport - (int)g_base_port;
            if (ch < 0 || ch > 255) {
                wa->other++;
                continue;
            }
            const int idx = g_dport_idx[ch];
            if (idx < 0) {                          /* 该 dport 不在 --chans 里 */
                wa->other++;
                continue;
            }
            /* 硬 steer 下队列与通道绑定：队列 q 只该收到 g_chans[q] 的包 */
            if (!wa->soft && idx != (int)wa->queue) {
                wa->other++;
                continue;
            }

            if (!pi.hdr_ok) {
                atomic_fetch_add(&chans[idx].bad, 1);
                if (atomic_exchange(&g_warned_bad, 1) == 0) {
                    fprintf(stderr,
                            "警告: 通道 %d 的载荷头不符（magic=0x%08x 长度=%u，期望 "
                            "magic=0x%08X 长度=%u）。已按坏包丢弃，请核对固件包格式。\n",
                            g_chans[idx], pi.magic, pi.plen, HEAD_MAGIC, FRAME_LEN);
                }
                continue;
            }

            uint64_t gap = 0;
            const enum verdict v = track_update(&chans[idx], pi.seq, &gap);

            if (idx == g_raw_chan && g_raw_fp != NULL) {
                /* payload 的前 32 B 是 MetaData，采样从 META_LEN 开始；
                 * 2048 个 I/Q 对（4096 个 int16）才是 8192 B 采样。 */
                const int16_t *sm = (const int16_t *)(pi.payload + META_LEN);
                const int      n  = (SAMPLE_LEN / 4) / g_raw_stride;
                static uint8_t buf[8 + 2048 * 4];   /* stride=1 时 2048 对 I/Q，8200 B */
                int16_t       *dst = (int16_t *)(buf + 8);

                memcpy(buf, &pi.seq, 8);
                for (int k = 0; k < n; k++) {
                    const int src = k * g_raw_stride;

                    dst[2 * k]     = sm[2 * src];
                    dst[2 * k + 1] = sm[2 * src + 1];
                }
                if (g_raw_no_seq)
                    fwrite(buf + 8, (size_t)(n * 4), 1, g_raw_fp);
                else
                    fwrite(buf, (size_t)(8 + n * 4), 1, g_raw_fp);
            }

            if (v == V_GAP) {                    /* 记下这次跳跃的时刻与大小 */
                const int k = atomic_fetch_add(&g_gevt_n[idx], 1) & (GAP_EVT_MAX - 1);

                g_gevt[idx][k].t_ns = now_mono_ns() -
                                      atomic_load_explicit(&g_t0_ns, memory_order_relaxed);
                g_gevt[idx][k].seq  = pi.seq;
                g_gevt[idx][k].size = gap;
            }

            if (v == V_RESTART)
                atomic_store_explicit(&g_last_reset_ns, now_mono_ns(),
                                      memory_order_relaxed);

            if (atomic_load_explicit(&g_first_pkt_ns, memory_order_relaxed) == 0) {
                uint64_t zero = 0;
                (void)atomic_compare_exchange_strong(&g_first_pkt_ns, &zero,
                                                     now_mono_ns());
            }
            atomic_store_explicit(&g_last_pkt_ns, now_mono_ns(),
                                  memory_order_relaxed);
        }

        rte_pktmbuf_free_bulk(bufs, n);
    }

    atomic_fetch_add(&g_other, wa->other);
    return 0;
}

/* ------------------------------------------------------------------ 打印 */

struct ch_snap {
    uint64_t recv, lost, gaps, maxgap, rounds, dup, bad;
    uint64_t first_seq, last_seq, expected;
    int      started;
};

static void snap_read(int c, struct ch_snap *s)
{
    struct ch_track *t = &chans[c];

    s->recv     = atomic_load_explicit(&t->recv, memory_order_relaxed);
    s->lost     = atomic_load_explicit(&t->lost, memory_order_relaxed);
    s->gaps     = atomic_load_explicit(&t->gaps, memory_order_relaxed);
    s->maxgap   = atomic_load_explicit(&t->maxgap, memory_order_relaxed);
    s->rounds   = atomic_load_explicit(&t->rounds, memory_order_relaxed);
    s->dup      = atomic_load_explicit(&t->dup, memory_order_relaxed);
    s->bad      = atomic_load_explicit(&t->bad, memory_order_relaxed);
    s->first_seq = atomic_load_explicit(&t->first_seq, memory_order_relaxed);
    s->last_seq  = atomic_load_explicit(&t->last_seq, memory_order_relaxed);
    s->expected  = atomic_load_explicit(&t->expected, memory_order_relaxed);
    s->started   = atomic_load_explicit(&t->started, memory_order_relaxed);
}

static uint64_t now_mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double now_mono(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static const char *fmt_chans(void)
{
    static char buf[256];
    size_t      off = 0;

    buf[0] = '\0';
    for (int i = 0; i < g_nchan && off + 8 < sizeof(buf); i++)
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, i ? ",%d" : "%d",
                                g_chans[i]);
    return buf;
}

/* 丢包率**上界**，与 capture_pipeline / pipeline.rs 里的
 *   ratio < (1 + ndropped) / nreceived
 * 一致：分子 +1 覆盖"序列末尾可能没被观测到的那一个"，分母用实收数而不是
 * 期望总数，所以结果是保守上界。lost=0 时得到 1/recv（而不是 0）——意思是
 * "在这么多包里，仍可能有 1 个没被看见"。 */
static double rate_of(uint64_t lost, uint64_t recv)
{
    return recv > 0 ? (double)(lost + 1) / (double)recv : 0.0;
}

static uint64_t mbuf_avail(void)
{
    return g_pool ? (uint64_t)rte_mempool_avail_count(g_pool) : 0;
}

static void print_header(void)
{
    printf("\n%7s %3s %10s %9s %9s %8s %7s %7s %12s %4s %4s %4s %5s\n",
           "t(s)", "ch", "recv", "pps", "lost", "dlost", "gaps",
           "maxgap", "rate_ub", "rnd", "dup", "bad", "free%");
}

static struct ch_snap g_prev[MAX_CHAN];
static int      g_gevt_seen[MAX_CHAN];   /* 实时告警用：上次已打印到哪 */
static double         g_prev_t;

/* 读一组端口计数器到 cur[]。print_port_tick 和启动基线快照共用。
 * 返回 0 = 成功。 */
static int read_port_counters(uint64_t cur[XS_N])
{
    struct rte_eth_stats st;

    if (g_xidx[XS_PHY_PKTS] < 0)
        return -1;

    const int nall = rte_eth_xstats_get_names(g_port, NULL, 0);
    if (nall <= 0)
        return -1;

    struct rte_eth_xstat *all = calloc((size_t)nall, sizeof(*all));
    if (all == NULL)
        return -1;
    if (rte_eth_xstats_get(g_port, all, (unsigned)nall) != nall) {
        free(all);
        return -1;
    }
    for (int k = 0; k < XS_N; k++) {
        if (g_xidx[k] >= 0 && g_xidx[k] < nall)
            cur[k] = all[g_xidx[k]].value;
    }
    free(all);

    if (rte_eth_stats_get(g_port, &st) == 0 && g_xidx[XS_IMISSED] < 0)
        cur[XS_IMISSED] = st.imissed;
    return 0;
}

/* 启动窗口（dev_start → flow 生效）里队列溢出的计数留在硬件里，清不掉。
 * 把增量基线定在 drain 之后，第一秒的 port 行才不把这批算成"本秒新增"。 */
static void snapshot_port_baseline(void)
{
    uint64_t cur[XS_N] = { 0 };

    if (read_port_counters(cur) == 0)
        memcpy(g_xprev, cur, sizeof(g_xprev));
}

/* 端口层增量：一眼看出这一秒是 PHY 丢、队列丢、还是上层收全了。 */
static void print_port_tick(void)
{
    uint64_t cur[XS_N] = { 0 };

    if (read_port_counters(cur) != 0)
        return;

    printf("        port: phy_rx=+%" PRIu64 " phy_discard=+%" PRIu64
           " out_of_buffer=+%" PRIu64 " imissed=+%" PRIu64 "\n",
           cur[XS_PHY_PKTS] - g_xprev[XS_PHY_PKTS],
           cur[XS_PHY_DISCARD] - g_xprev[XS_PHY_DISCARD],
           cur[XS_OOB] - g_xprev[XS_OOB],
           cur[XS_IMISSED] - g_xprev[XS_IMISSED]);

    for (int k = 0; k < XS_N; k++)
        g_xprev[k] = cur[k];
}

static void print_tick(double t)
{
    const uint64_t mb = mbuf_avail();

    /* pps = 本轮累计包数 ÷ 本轮已过时间。
     * "本轮"从最近一次编号归零（或本次运行的首个有效包）算起：
     *   - 分母不含等待 StreamStart 的空窗；
     *   - 是累计平均而不是区间瞬时，tick 间隔抖动不会让数字跳；
     *   - 归零后分子分母同时从零开始。 */
    const uint64_t t0n  = atomic_load_explicit(&g_t0_ns, memory_order_relaxed);
    const uint64_t rstn = atomic_load_explicit(&g_last_reset_ns, memory_order_relaxed);
    const uint64_t fpn  = atomic_load_explicit(&g_first_pkt_ns, memory_order_relaxed);
    const uint64_t stn  = rstn > fpn ? rstn : fpn;
    double active = t;

    if (stn > t0n) {
        const double st = (double)(stn - t0n) * 1e-9;

        if (t > st + 1e-9)
            active = t - st;
    }

    printf("\n--- t = %7.2f s ---------------------------------------------------------"
           "----------------  mbuf_avail=%" PRIu64 "\n", t, mb);

    print_header();     /* 每个 tick 都带列名：滚动日志里不用往上翻 */

    for (int c = 0; c < g_nchan; c++) {
        struct ch_snap s;
        snap_read(c, &s);

        /* dlost 仍是区间增量（用来定位突发）；pps 用本轮累计平均 */
        const uint64_t dlost = s.lost >= g_prev[c].lost
                                   ? s.lost - g_prev[c].lost : s.lost;
        const double   pps   = (s.started && active > 1e-9)
                                   ? (double)s.recv / active : 0.0;

        if (!s.started) {
            if (!g_quiet)
                printf("%7.2f %3d %10s %9s %9s %8s %7s %7s %12s %4s %4s %4s %5s\n",
                       t, g_chans[c], "-", "-", "-", "-", "-", "-", "(无数据)",
                       "-", "-", "-", "-");
            g_prev[c] = s;
            continue;
        }

        printf("%7.2f %3d %10" PRIu64 " %9.2f %9" PRIu64 " %8" PRIu64 " %7" PRIu64
               " %7" PRIu64 " %12.3e %4" PRIu64 " %4" PRIu64 " %4" PRIu64 " %5.1f\n",
               t, g_chans[c], s.recv, pps, s.lost, dlost, s.gaps, s.maxgap,
               rate_of(s.lost, s.recv), s.rounds, s.dup, s.bad,
               (double)mb * 100.0 / (double)NB_MBUF);

        if (g_csv) {
            fprintf(g_csv, "%" PRId64 ",%.3f,%d,%" PRIu64 ",%.0f,%" PRIu64 ",%" PRIu64
                    ",%" PRIu64 ",%.9e,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                    (int64_t)time(NULL), t, g_chans[c], s.recv, pps, s.lost, s.gaps,
                    s.maxgap, rate_of(s.lost, s.recv), s.rounds, s.dup, s.bad, mb);
        }

        g_prev[c]  = s;
    }

    /* 新发生的跳跃按设计要立刻报到 stderr：突发丢包的时刻要和系统事件
     * （dmesg、top、别的进程）对得上，只在汇总里打印会错过现场。
     * 眼下逐条明细关着（见下），这里只推进 g_gevt_seen。 */
    for (int c = 0; c < g_nchan; c++) {
        const int n = atomic_load_explicit(&g_gevt_n[c], memory_order_relaxed);

        if (n == g_gevt_seen[c])
            continue;

        /* 逐条 [gap] 明细当前关着：一次突发能刷几百行 stderr，把汇总行冲掉。
         * gaps/maxgap 两列和 Ctrl-C 汇总照旧。要恢复现场明细，把下面整段
         * （连同 cnt/show 两个变量）解注释。
        const int cnt  = n - g_gevt_seen[c];
        const int show = cnt < 4 ? cnt : 4;

        for (int i = 0; i < show; i++) {
            const int k = (g_gevt_seen[c] + i) & (GAP_EVT_MAX - 1);

            fprintf(stderr, "[gap] t=%.3fs ch%d seq=%" PRIu64 " size=%" PRIu64 "\n",
                    (double)g_gevt[c][k].t_ns * 1e-9, g_chans[c],
                    g_gevt[c][k].seq, g_gevt[c][k].size);
        }
        if (cnt > show)
            fprintf(stderr, "[gap] ... ch%d 另有 %d 次未显示\n", g_chans[c], cnt - show);
        */
        g_gevt_seen[c] = n;
    }

    print_port_tick();
    g_prev_t = t;
}

static void init_xstat_idx(uint16_t port)
{
    const int n = rte_eth_xstats_get_names(port, NULL, 0);
    if (n <= 0)
        return;

    struct rte_eth_xstat_name *names = calloc((size_t)n, sizeof(*names));
    if (!names)
        return;
    if (rte_eth_xstats_get_names(port, names, (unsigned)n) == n) {
        for (int k = 0; k < XS_N; k++) {
            for (int i = 0; i < n; i++) {
                if (!strcmp(names[i].name, XS_NAME[k])) {
                    g_xidx[k] = i;
                    break;
                }
            }
        }
    }
    free(names);
}

/* 打印网卡全部计数器：排查时用来区分 PHY 错误 / 网卡丢弃 / 主机队列问题。 */
static void dump_all_xstats(uint16_t port)
{
    const int n = rte_eth_xstats_get_names(port, NULL, 0);
    if (n <= 0) {
        printf("（该设备没有 xstats）\n");
        return;
    }

    struct rte_eth_xstat_name *names = calloc((size_t)n, sizeof(*names));
    struct rte_eth_xstat      *vals  = calloc((size_t)n, sizeof(*vals));

    if (!names || !vals) {
        free(names);
        free(vals);
        return;
    }
    if (rte_eth_xstats_get_names(port, names, (unsigned)n) == n &&
        rte_eth_xstats_get(port, vals, (unsigned)n) > 0) {
        for (int i = 0; i < n; i++) {
            if (vals[i].value != 0)
                printf("  %-40s %" PRIu64 "\n", names[i].name, vals[i].value);
        }
    }
    free(names);
    free(vals);
}

/* 端口级计数：这些才是"主机侧丢包"的证据。 */
static void print_port_stats(uint16_t port)
{
    struct rte_eth_stats st;

    if (rte_eth_stats_get(port, &st) == 0) {
        printf("  端口计数  rx_packets=%" PRIu64 "  rx_bytes=%" PRIu64
               "  imissed=%" PRIu64 "  rx_nombuf=%" PRIu64 "  ierrors=%" PRIu64 "\n",
               st.ipackets, st.ibytes, st.imissed - g_xprev[XS_IMISSED],
               st.rx_nombuf, st.ierrors);
    } else {
        printf("  端口计数  读取失败\n");
    }

    const int n = rte_eth_xstats_get_names(port, NULL, 0);
    if (n <= 0)
        return;

    struct rte_eth_xstat_name *names = calloc((size_t)n, sizeof(*names));
    struct rte_eth_xstat      *vals  = calloc((size_t)n, sizeof(*vals));

    if (!names || !vals)
        goto out;
    if (rte_eth_xstats_get_names(port, names, (unsigned)n) != n)
        goto out;

    const int m = rte_eth_xstats_get(port, vals, (unsigned)n);
    if (m <= 0)
        goto out;

    /* 只挑与"接收侧丢包"直接相关的计数器 */
    static const char *want[] = {
        "rx_out_of_buffer", "rx_steer_missed_packets", "rx_discards_phy",
        "rx_errors_phy", "rx_crc_errors_phy", "rx_symbol_errors_phy",
        "rx_packets_phy", "rx_bytes_phy",
    };

    printf("  hw 计数  ");
    for (size_t w = 0; w < sizeof(want) / sizeof(want[0]); w++) {
        for (int i = 0; i < m && i < n; i++) {
            if (strstr(names[i].name, want[w]) == NULL)
                continue;
            /* 减去启动窗口（flow 生效前）的基线，否则这里显示的是开机以来的累计值 */
            uint64_t v = vals[i].value;

            if (strcmp(names[i].name, "rx_out_of_buffer") == 0)
                v -= g_xprev[XS_OOB];
            else if (strcmp(names[i].name, "rx_discards_phy") == 0)
                v -= g_xprev[XS_PHY_DISCARD];
            else if (strcmp(names[i].name, "rx_packets_phy") == 0)
                v -= g_xprev[XS_PHY_PKTS];
            printf("%s=%" PRIu64 "  ", names[i].name, v);
        }
    }
    printf("\n");

out:
    free(names);
    free(vals);
}

static void print_summary(double elapsed, uint16_t port)
{
    uint64_t recv = 0, lost = 0, gaps = 0, maxgap = 0, rounds = 0, dup = 0, bad = 0;
    uint64_t first = 0, last = 0, ref_span = 0;
    int      any_started = 0, ref_chan = -1;

    printf("\n================= 汇总 =================\n");
    printf("%-10s %12s %12s %8s %8s %12s %6s %6s %6s\n",
           "ch", "recv", "lost", "gaps", "maxgap", "rate_ub", "rnd", "dup", "bad");
    printf("---------------------------------------------------------------"
           "----------------------\n");

    for (int c = 0; c < g_nchan; c++) {
        struct ch_snap s;
        snap_read(c, &s);

        if (s.started) {
            if (ref_chan < 0) {              /* first/last 只取同一通道，跨通道不可比 */
                first     = s.first_seq;
                last      = s.last_seq;
                ref_chan  = g_chans[c];
            }
            any_started = 1;
            printf("%-10d %12" PRIu64 " %12" PRIu64 " %8" PRIu64 " %8" PRIu64
                   " %12.3e %6" PRIu64 " %6" PRIu64 " %6" PRIu64 "\n",
                   g_chans[c], s.recv, s.lost, s.gaps, s.maxgap,
                   rate_of(s.lost, s.recv), s.rounds, s.dup, s.bad);
        } else {
            printf("%-10d %12s %12s %8s %8s %12s %6s %6s %6s\n",
                   g_chans[c], "-", "-", "-", "-", "(无数据)", "-", "-", "-");
        }

        recv += s.recv;
        lost += s.lost;
        gaps += s.gaps;
        if (s.maxgap > maxgap)
            maxgap = s.maxgap;
        if (s.rounds > rounds)
            rounds = s.rounds;               /* 各通道轮次相同，取最大作参考 */
        dup      += s.dup;
        bad      += s.bad;
    }

    /* 吞吐分母 = 从"统计起点"到结束。统计起点取首包与最近一次编号归零
     * 的较晚者：先起接收后开流时排除等待空窗；中途归零清零后只算这一轮。 */
    const uint64_t start_ns = g_last_reset_ns > g_first_pkt_ns
                                  ? g_last_reset_ns : g_first_pkt_ns;
    const double start_t  = start_ns != 0
                                ? (double)(start_ns - atomic_load(&g_t0_ns)) * 1e-9 : -1.0;
    const double active   = (start_t >= 0.0 && elapsed > start_t)
                                ? elapsed - start_t : elapsed;
    const double pps      = active > 0.0 ? (double)recv / active : 0.0;
    const double gbps     = pps * (double)FRAME_LEN * 8.0 / 1e9;
    const double nominal_per_ch = g_sps / (double)(SAMPLE_LEN / BYTES_PER_SAMPLE);
    const double nominal  = nominal_per_ch * (double)g_nchan;
    const double fill_pct = nominal > 0.0 ? pps / nominal * 100.0 : 0.0;
    const uint64_t other  = atomic_load(&g_other);

    printf("---------------------------------------------------------------"
           "----------------------\n");
    printf("%-10s %12" PRIu64 " %12" PRIu64 " %8" PRIu64 " %8" PRIu64
           " %12.3e %6" PRIu64 " %6" PRIu64 " %6" PRIu64 "\n",
           "合计", recv, lost, gaps, maxgap, rate_of(lost, recv), rounds, dup, bad);

    printf("\n运行时长        %.2f s（有效统计 %.2f s）\n", elapsed, active);
    if (start_t >= 0.0 && active < elapsed - 0.5) {
        if (g_last_reset_ns > g_first_pkt_ns)
            printf("                最近一轮编号归零在 %.2f s，之前的统计已清零\n", start_t);
        else
            printf("                首个有效包在 %.2f s 到达，之前是等待数据流，未计入吞吐\n",
                   start_t);
    }
    printf("通道/队列       %d 路（ch %s，UDP %u+ch），%s\n", g_nchan, fmt_chans(),
           g_base_port, g_soft_steer ? "单队列软分流" : "每通道一队列");
    printf("收到            %" PRIu64 " 包（%.2f GiB 采样数据）\n", recv,
           (double)recv * (double)SAMPLE_LEN / (1024.0 * 1024.0 * 1024.0));
    printf("丢包(编号跳跃)  %" PRIu64 " 包（%d 段跳跃，最大一次 %" PRIu64 "）\n",
           lost, (int)gaps, maxgap);
    printf("丢包率上界      %.3e  ((1+lost)/recv；lost=0 时即为 1/recv)\n",
           rate_of(lost, recv));
    printf("轮次(归零)      %" PRIu64 " 次（每次编号归零，统计清零重新开始）\n", rounds);
    printf("重复包          %" PRIu64 " 个\n", dup);
    printf("坏头            %" PRIu64 " 个；非目标包 %" PRIu64 " 个\n", bad, other);
    printf("吞吐            %.2f Gbps，%.3f Mpps，%.1f%% 的名义速率"
           "（%.0f MSps × %d 路 / 每包 %u 点）\n",
           gbps, pps / 1e6, fill_pct, g_sps / 1e6, g_nchan,
           SAMPLE_LEN / BYTES_PER_SAMPLE);
    if (any_started) {
        /* 帧跨度速率 = pkt_cnt 跨度 / (末包-首包)。
         * 分子取自包头编号，与接收侧丢包无关；分母只覆盖首末包之间，
         * 因此这才是"板卡每秒产生多少帧"，不是"本机每秒收到多少帧"。 */
        const uint64_t fpn = atomic_load(&g_first_pkt_ns);
        const uint64_t lpn = atomic_load(&g_last_pkt_ns);
        if (lpn > fpn) {
            const double span_s = (double)(lpn - fpn) * 1e-9;
            const uint64_t span = last - first + 1;
            printf("帧跨度速率      %.3f pps（跨度 %" PRIu64 " 帧 / %.3f s；"
                   "用包头编号，不受接收丢包影响）\n",
                   (double)span / span_s, span, span_s);
        }
        ref_span = lost && recv ? 0 : 0;     /* 占位，避免未使用告警 */
        (void)ref_span;
        printf("编号范围        ch%d: 首 %" PRIu64 " / 末 %" PRIu64 "（跨度 %" PRIu64 "）\n",
               ref_chan, first, last, last - first + 1);
        printf("                注: 每通道 pkt_cnt 独立计数，这里只列一个通道作参照\n");
    }
    /* 时间线：链路随机误码是分散的单包丢；CPU 抢占/队列溢出是某一刻的集中丢。
     * 看这里的时间分布就能区分，配合 port 行的 out_of_buffer 增量更直接。 */
    int any_evt = 0;

    for (int c = 0; c < g_nchan; c++)
        if (atomic_load(&g_gevt_n[c]) > 0)
            any_evt = 1;

    if (any_evt) {
        printf("\n丢包事件（每通道最近 5 次：时刻 / 编号 / 跳跃量）\n");
        for (int c = 0; c < g_nchan; c++) {
            const int n = atomic_load(&g_gevt_n[c]);

            if (n == 0)
                continue;

            const int show = n < 5 ? n : 5;

            printf("  ch%-2d", g_chans[c]);
            for (int i = 0; i < show; i++) {
                const int k = (n - show + i) & (GAP_EVT_MAX - 1);

                printf("  %.3fs/%" PRIu64 "/g%" PRIu64,
                       (double)g_gevt[c][k].t_ns * 1e-9,
                       g_gevt[c][k].seq, g_gevt[c][k].size);
            }
            printf("\n");
        }
    }

    printf("\n");
}

/* 把"编号跳跃"与"主机侧丢包"对比，给出归因结论。 */
static void print_verdict(uint16_t port, uint64_t lost)
{
    struct rte_eth_stats st;
    uint64_t host = 0;

    if (rte_eth_stats_get(port, &st) == 0)
        host = st.imissed + st.rx_nombuf;

    printf("归因\n");
    printf("  帧编号跳跃总数（估计丢包）: %" PRIu64 "\n", lost);
    printf("  主机侧丢包（imissed + rx_nombuf）: %" PRIu64 "\n", host);

    if (lost == 0 && host == 0) {
        printf("  判定: 未观察到丢包（编号连续，主机侧计数为零）\n");
    } else if (host >= lost) {
        printf("  判定: 跳跃基本可由主机接收侧解释 —— 瓶颈在本机网卡/mbuf/队列，"
               "不是板卡或链路。先加大 mbuf/描述符或多核，再测。\n");
    } else {
        printf("  判定: 残余跳跃 %" PRIu64 " 包发生在主机接收路径之外（板卡停发、"
               "链路丢包或 PFC/拥塞）。主机侧只解释了 %" PRIu64 " 包。\n",
               lost - host, host);
        printf("        注意 imissed 是端口级累计量，跨通道求和后与总跳跃比较；"
               "若某通道跳跃而 imissed 不涨，优先怀疑该通道链路/发送端。\n");
    }
    printf("\n");
}

/* ------------------------------------------------------------------ 自检 */
/* 判定逻辑与 DPDK 无关，这里用合成序列验证，不需要 root/大页/网卡。 */

static int g_fail;

#define CHECK(cond, fmt, ...)                                                   \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                         \
            g_fail++;                                                           \
        }                                                                       \
    } while (0)

static void selftest_case(const char *name, const uint64_t *seqs, size_t n,
                          uint64_t want_recv, uint64_t want_lost, uint64_t want_gaps,
                          uint64_t want_maxgap, uint64_t want_rounds, uint64_t want_dup)
{
    struct ch_track t;

    memset(&t, 0, sizeof(t));

    for (size_t i = 0; i < n; i++) {
        uint64_t gap;

        (void)track_update(&t, seqs[i], &gap);
    }

    const uint64_t recv     = atomic_load(&t.recv);
    const uint64_t lost     = atomic_load(&t.lost);
    const uint64_t gaps     = atomic_load(&t.gaps);
    const uint64_t maxgap   = atomic_load(&t.maxgap);
    const uint64_t rounds   = atomic_load(&t.rounds);
    const uint64_t dup      = atomic_load(&t.dup);

    printf("  %-34s recv=%-4" PRIu64 " lost=%-6" PRIu64 " gaps=%-3" PRIu64
           " max=%-6" PRIu64 " rnd=%-2" PRIu64 " dup=%-2" PRIu64 " %s\n",
           name, recv, lost, gaps, maxgap, rounds, dup,
           (recv == want_recv && lost == want_lost && gaps == want_gaps &&
            maxgap == want_maxgap && rounds == want_rounds && dup == want_dup)
               ? "OK" : "MISMATCH");

    CHECK(recv == want_recv, "%s: recv %" PRIu64 " != %" PRIu64, name, recv, want_recv);
    CHECK(lost == want_lost, "%s: lost %" PRIu64 " != %" PRIu64, name, lost, want_lost);
    CHECK(gaps == want_gaps, "%s: gaps %" PRIu64 " != %" PRIu64, name, gaps, want_gaps);
    CHECK(maxgap == want_maxgap, "%s: maxgap %" PRIu64 " != %" PRIu64, name, maxgap, want_maxgap);
    CHECK(rounds == want_rounds, "%s: rounds %" PRIu64 " != %" PRIu64, name,
          rounds, want_rounds);
    CHECK(dup == want_dup, "%s: dup %" PRIu64 " != %" PRIu64, name, dup, want_dup);
}

static int run_selftest(void)
{
    static const uint64_t s_basic[]  = { 0, 1, 2, 5, 6, 6 };
    static const uint64_t s_zero[]   = { 0, 1, 2, 5, 0, 1, 2 };
    static const uint64_t s_mid[]    = { 100, 101, 102, 103 };
    static const uint64_t s_mid2[]   = { 100, 101, 102, 103, 50, 51 };
    static const uint64_t s_multi[]  = { 0, 1, 0, 1, 0, 1 };
    static const uint64_t s_big[]    = { 0, 10000 };

    printf("判定逻辑自检（不需要 DPDK 初始化）\n");
    /*                                                    recv lost gaps max rnd dup */
    selftest_case("正常+跳跃+重复", s_basic, 6, 5, 2, 1, 2, 0, 1);
    selftest_case("归零清零（第一段 4 包丢失）", s_zero, 7, 3, 0, 0, 0, 1, 0);
    selftest_case("中途接入（首包非 0）", s_mid, 4, 4, 0, 0, 0, 0, 0);
    selftest_case("回退到非 0 也清零", s_mid2, 6, 2, 0, 0, 0, 1, 0);
    selftest_case("连续两轮归零", s_multi, 6, 2, 0, 0, 0, 2, 0);
    selftest_case("大跳跃", s_big, 2, 2, 9999, 1, 9999, 0, 0);

    printf("%s\n\n", g_fail == 0 ? "自检通过" : "自检失败");
    return g_fail == 0 ? 0 : 1;
}

#undef CHECK

/* ------------------------------------------------------------------ 参数 */

static const char *USAGE =
    "用法: pktloss [EAL 参数 ...] -- [选项 ...]\n"
    "\n"
    "EAL 参数（示例）: -l 0-8 -n 4 --allow=0000:01:00.0 --socket-mem=2048\n"
    "\n"
    "选项:\n"
    "  -p, --pci ADDR        DPDK 设备 PCI 地址（默认取第一个可用端口）\n"
    "  -b, --base-port N     UDP 目的端口基址（默认 4000，通道号 = dport - base）\n"
    "      --chans LIST     要收的通道（默认 0-7）。支持 0-3、0,2,4,6、0,2-3。\n"
    "                       通道号 = UDP 目的端口 - base；只配列出的队列和 flow。\n"
    "  -d, --duration SEC    运行秒数（默认 0 = 直到 Ctrl-C）\n"
    "  -r, --sps HZ          采样率，用于吞吐百分比（默认 320e6）\n"
    "  -i, --interval SEC    打印间隔（默认 1.0）\n"
    "  -c, --csv PATH        追加机器可读统计到文件\n"
    "  -s, --soft-steer      单队列 + 软件按 dport 分流（网卡不支持 dport flow 或\n"
    "                        用 pcap PMD 自测时使用）\n"
    "  -q, --quiet           不打印无数据的通道行\n"
    "      --no-drain        跳过启动窗口排空（pcap 回放这类一次性流量用，\n"
    "                        否则 drain 会把回放数据当残留全丢掉）\n"
    "      --dump N          打印前 N 个包的 mbuf 原始头部（排障用）\n"
    "      --sched-fifo N    worker 用 SCHED_FIFO N（防 CPU 抢占造成的集中丢包）\n"
    "      --raw-ch N        记录通道 N 的时域降采样（找 PPS 脉冲用）\n"
    "      --raw-stride N    每 N 个采样点存 1 个（默认 32）\n"
    "      --raw-file PATH   输出：每包 seq(8B) + (2048/N) 个 I/Q(int16 x2)\n"
    "      --xstats          启动后 dump 全部网卡计数器并退出（排查链路/丢包用）\n"
    "      --selftest        只跑判定逻辑自检（在 EAL 之前处理）\n"
    "  -h, --help            显示本帮助\n"
    "\n"
    "判据: seq==expected 正常 / seq>expected 丢 seq-expected 个 /\n"
    "      seq==last 重复 / seq<expected 且 !=last → 流重启，重置基线\n"
    "丢包率: lost/(lost+recv)\n";

/* 通道列表：支持 0-7 / 0,2,4,6 / 0,2-3。排序去重，重复直接报错（两个队列
 * 争同一路会让编号序列交错，判据失效）。 */
static void parse_chans(const char *s)
{
    int        list[MAX_CHAN];
    int        n = 0;
    const char *p = s;

    if (s == NULL || *s == '\0') {
        fprintf(stderr, "--chans 不能为空\n");
        exit(EXIT_FAILURE);
    }

    while (*p != '\0') {
        char *end = NULL;
        const long a = strtol(p, &end, 10);

        if (end == p) {
            fprintf(stderr, "--chans 解析失败（\"%s\"），格式如 0-7 或 0,2,4,6\n", p);
            exit(EXIT_FAILURE);
        }

        long b = a;

        if (*end == '-') {
            p = end + 1;
            b = strtol(p, &end, 10);
            if (end == p) {
                fprintf(stderr, "--chans 区间缺少终点: %s\n", s);
                exit(EXIT_FAILURE);
            }
        }
        if (a < 0 || b < a || b > 255) {
            fprintf(stderr, "--chans 区间非法: %ld-%ld\n", a, b);
            exit(EXIT_FAILURE);
        }
        for (long c = a; c <= b; c++) {
            if (n >= MAX_CHAN) {
                fprintf(stderr, "--chans 最多 %d 个通道\n", MAX_CHAN);
                exit(EXIT_FAILURE);
            }
            list[n++] = (int)c;
        }

        if (*end == ',') {
            p = end + 1;
            if (*p == '\0') {
                fprintf(stderr, "--chans 末尾多了逗号\n");
                exit(EXIT_FAILURE);
            }
        } else if (*end == '\0') {
            break;
        } else {
            fprintf(stderr, "--chans 解析失败（\"%s\"）\n", end);
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 1; i < n; i++) {            /* 插入排序，规模 <= 64 */
        const int v = list[i];
        int       j = i - 1;

        while (j >= 0 && list[j] > v) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = v;
    }

    g_nchan = 0;
    for (int i = 0; i < n; i++) {
        if (g_nchan > 0 && g_chans[g_nchan - 1] == list[i]) {
            fprintf(stderr, "--chans 里通道 %d 重复\n", list[i]);
            exit(EXIT_FAILURE);
        }
        g_chans[g_nchan++] = list[i];
    }
}

/* dport - base → g_chans 下标；-1 表示不收这个端口。 */
static void build_dport_map(void)
{
    for (int i = 0; i < 256; i++)
        g_dport_idx[i] = -1;

    for (int i = 0; i < g_nchan; i++) {
        if ((uint32_t)g_base_port + (uint32_t)g_chans[i] > 65535u) {
            fprintf(stderr, "通道 %d 的 UDP 端口超出 65535\n", g_chans[i]);
            exit(EXIT_FAILURE);
        }
        g_dport_idx[g_chans[i]] = i;
    }
}

/* 手写解析：rte_eal_init 之后 argv 已被重排，getopt 的 optind 语义在这里
 * 容易踩坑（会吞掉第一个参数），参数又少，直接遍历更清楚。 */
static const char *optval(int argc, char **argv, int *i, const char *name)
{
    const size_t n = strlen(name);
    const char  *a = argv[*i];

    if (strncmp(a, name, n) != 0)
        return NULL;
    if (a[n] == '=')
        return a + n + 1;
    if (a[n] == '\0') {
        if (*i + 1 >= argc) {
            fprintf(stderr, "%s 缺少取值\n", name);
            exit(EXIT_FAILURE);
        }
        return argv[++(*i)];
    }
    return NULL;
}

static double parse_double(const char *s, const char *what)
{
    char  *end = NULL;
    double v   = strtod(s, &end);

    if (end == s || *end != '\0' || v < 0.0) {
        fprintf(stderr, "%s 的取值非法: %s\n", what, s);
        exit(EXIT_FAILURE);
    }
    return v;
}

static void parse_args(int argc, char **argv)
{
    parse_chans("0-7");                     /* 默认收全部 8 路，--chans 可覆盖 */

    for (int i = 0; i < argc; i++) {
        const char *v;

        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("%s", USAGE);
            exit(EXIT_SUCCESS);
        } else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--soft-steer")) {
            g_soft_steer = 1;
        } else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) {
            g_quiet = 1;
        } else if (!strcmp(argv[i], "--no-drain")) {
            g_drain_start = 0;
        } else if ((v = optval(argc, argv, &i, "--pci")) || (v = optval(argc, argv, &i, "-p"))) {
            snprintf(g_pci, sizeof(g_pci), "%s", v);
        } else if ((v = optval(argc, argv, &i, "--base-port")) ||
                   (v = optval(argc, argv, &i, "-b"))) {
            g_base_port = (uint16_t)strtoul(v, NULL, 10);
        } else if ((v = optval(argc, argv, &i, "--sched-fifo"))) {
            g_rt_prio = atoi(v);
            if (g_rt_prio < 0 || g_rt_prio > 99) {
                fprintf(stderr, "--sched-fifo 必须是 0..99（0 = 不设）\n");
                exit(EXIT_FAILURE);
            }
        } else if ((v = optval(argc, argv, &i, "--raw-ch"))) {
            g_raw_chan = atoi(v);
        } else if ((v = optval(argc, argv, &i, "--raw-stride"))) {
            g_raw_stride = atoi(v);
            if (g_raw_stride < 1 || g_raw_stride > 1024) {
                fprintf(stderr, "--raw-stride 必须在 1..1024\n");
                exit(EXIT_FAILURE);
            }
        } else if ((v = optval(argc, argv, &i, "--raw-file"))) {
            g_raw_fp = fopen(v, "wb");
            if (g_raw_fp == NULL) {
                fprintf(stderr, "打不开 %s: %s\n", v, strerror(errno));
                exit(EXIT_FAILURE);
            }
            setvbuf(g_raw_fp, NULL, _IOFBF, 4 << 20);
        } else if (!strcmp(argv[i], "--raw-no-seq")) {
            g_raw_no_seq = 1;
        } else if (!strcmp(argv[i], "--xstats")) {
            g_show_xstats = 1;
        } else if ((v = optval(argc, argv, &i, "--dump"))) {
            g_dump_left = atoi(v);
        } else if ((v = optval(argc, argv, &i, "--chans"))) {
            parse_chans(v);
        } else if ((v = optval(argc, argv, &i, "--duration")) ||
                   (v = optval(argc, argv, &i, "-d"))) {
            g_duration = parse_double(v, "--duration");
        } else if ((v = optval(argc, argv, &i, "--sps")) ||
                   (v = optval(argc, argv, &i, "-r"))) {
            g_sps = parse_double(v, "--sps");
        } else if ((v = optval(argc, argv, &i, "--interval")) ||
                   (v = optval(argc, argv, &i, "-i"))) {
            g_interval = parse_double(v, "--interval");
        } else if ((v = optval(argc, argv, &i, "--csv")) ||
                   (v = optval(argc, argv, &i, "-c"))) {
            snprintf(g_csv_path, sizeof(g_csv_path), "%s", v);
        } else if (argv[i][0] == '#') {
            /* 从文档里整行复制时，"# 终端 A ..." 这种行内注释可能被当成参数
             * 传进来（全角空格等）。看到 # 就把剩余参数都当注释。 */
            break;
        } else {
            fprintf(stderr, "未知参数: %s（EAL 参数请放在 -- 之前）\n", argv[i]);
            exit(EXIT_FAILURE);
        }
    }

    if (g_interval < 0.05) {
        fprintf(stderr, "--interval 不能小于 0.05 s\n");
        exit(EXIT_FAILURE);
    }

    build_dport_map();
}

static void on_signal(int sig)
{
    (void)sig;
    atomic_store(&g_stop, 1);
}

/* ------------------------------------------------------------------ 主流程 */

/* 每个通道一条 flow：IPv4/UDP，dst_port = base+ch → queue ch。
 * 只匹配 dport，不匹配 IP/MAC —— 板卡的源/目的地址由 XGbeCfgSingle 装订，
 * 这里不假设，避免地址改了要改程序。 */
static int setup_flows(uint16_t port, int nq)
{
    struct rte_flow_error err;

    if (rte_flow_flush(port, &err) != 0) {
        fprintf(stderr, "提示: rte_flow_flush 失败: %s\n", err.message ? err.message : "?");
    }

    for (int ch = 0; ch < nq; ch++) {
        struct rte_flow_attr attr = {
            .ingress = 1,
            .group   = 0,
            .priority = 0,
        };
        struct rte_flow_item_udp udp_spec = {
            .hdr = {
                .src_port   = 0,
                .dst_port   = htons((uint16_t)(g_base_port + g_chans[ch])),
                .dgram_len  = 0,
                .dgram_cksum = 0,
            },
        };
        const struct rte_flow_item_udp udp_mask = {
            .hdr = {
                .src_port   = 0,
                .dst_port   = 0xffff,
                .dgram_len  = 0,
                .dgram_cksum = 0,
            },
        };
        const struct rte_flow_item pattern[] = {
            { .type = RTE_FLOW_ITEM_TYPE_ETH },
            { .type = RTE_FLOW_ITEM_TYPE_IPV4 },
            { .type = RTE_FLOW_ITEM_TYPE_UDP, .spec = &udp_spec, .mask = &udp_mask },
            { .type = RTE_FLOW_ITEM_TYPE_END },
        };
        struct rte_flow_action_queue qconf = { .index = (uint16_t)ch };
        const struct rte_flow_action actions[] = {
            { .type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &qconf },
            { .type = RTE_FLOW_ACTION_TYPE_END },
        };

        if (rte_flow_create(port, &attr, pattern, actions, &err) == NULL) {
            fprintf(stderr,
                    "创建 dport=%u → queue %d 的 flow 失败: %s (%s)\n"
                    "  网卡可能不支持按 UDP 目的端口精确分流；可改用 --soft-steer "
                    "（单队列软分流，代价是单核处理全部通道）。\n",
                    (unsigned)(g_base_port + g_chans[ch]), g_chans[ch],
                    err.message ? err.message : "unknown",
                    err.type == RTE_FLOW_ERROR_TYPE_ACTION ? "action" : "item/attr");
            return -1;
        }
    }
    return 0;
}

static int port_init(uint16_t port)
{
    struct rte_eth_dev_info info;
    struct rte_eth_conf     conf;
    const int               nq = g_soft_steer ? 1 : g_nchan;

    memset(&conf, 0, sizeof(conf));
    conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;   /* 不用 RSS：分流靠 flow/软件 */
    conf.rxmode.mtu     = LINK_MTU;
    conf.rxmode.offloads = 0;
    conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

    if (rte_eth_dev_info_get(port, &info) != 0)
        rte_exit(EXIT_FAILURE, "rte_eth_dev_info_get(%u) 失败\n", port);

    if ((int)info.max_rx_queues < nq)
        rte_exit(EXIT_FAILURE, "设备只支持 %u 个 RX 队列，需要 %d 个\n",
                 info.max_rx_queues, nq);

    if (rte_eth_dev_configure(port, (uint16_t)nq, 0, &conf) != 0)
        rte_exit(EXIT_FAILURE, "rte_eth_dev_configure 失败（%d 个 RX 队列）\n", nq);

    if (rte_eth_dev_set_mtu(port, LINK_MTU) != 0)
        fprintf(stderr, "提示: 设置 MTU %d 失败（继续，pcap/虚拟设备可能不支持）\n",
                LINK_MTU);

    const int      socket_id = rte_eth_dev_socket_id(port);
    const unsigned qsock     = socket_id >= 0 ? (unsigned)socket_id : 0u;

    for (int q = 0; q < nq; q++) {
        struct rte_eth_rxconf rxq;

        memset(&rxq, 0, sizeof(rxq));
        if (rte_eth_rx_queue_setup(port, (uint16_t)q, RX_DESC, qsock,
                                   &rxq, g_pool) != 0)
            rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup(queue %d) 失败\n", q);
    }

    if (rte_eth_dev_start(port) != 0)
        rte_exit(EXIT_FAILURE, "rte_eth_dev_start 失败\n");

    if (!g_soft_steer && setup_flows(port, nq) != 0)
        rte_exit(EXIT_FAILURE, "硬件分流配置失败，未开始收包。\n");

    /* 100G 光口通常已 link up；这里只做提示，不阻塞。 */
    struct rte_eth_link link;

    memset(&link, 0, sizeof(link));
    if (rte_eth_link_get_nowait(port, &link) == 0) {
        if (link.link_status)
            printf("端口 %u: link up, %u Mbps, %s\n", port, link.link_speed,
                   link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX ? "full" : "half");
        else
            printf("端口 %u: link down（等对端/光模块），仍继续收包\n", port);
    }
    return 0;
}

int main(int argc, char **argv)
{
    /* --selftest / --help 必须在 EAL 之前拦下：EAL 自己也有 -h/--help，
     * 不拦就会打印 DPDK 的参数列表。 */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest"))
            return run_selftest();
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("%s", USAGE);
            return 0;
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 自己的参数用 "--" 与 EAL 参数分开。只把 "--" 及之前交给 EAL，自定义
     * 参数直接从 "--" 之后取 —— 不依赖 rte_eal_init 的返回语义：它把
     * argv[0] 当程序名保留、且返回的个数不含程序名，套用示例里的
     * "argc -= ret; argv += ret" 会吃到程序名。 */
    int dd = -1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--")) {
            dd = i;
            break;
        }
    }

    const int eal_argc = dd >= 0 ? dd + 1 : argc;
    const int ret      = rte_eal_init(eal_argc, argv);

    if (ret < 0)
        rte_exit(EXIT_FAILURE,
                 "\nEAL 初始化失败。检查: 大页是否配置（sudo ./setup.sh）、"
                 "-l 指定的 lcore 是否存在、--allow 的 PCI 地址是否正确。\n");

    /* 任何 rte_exit 路径下都先让 worker 停下，否则退出会卡住（见上面的注释） */
    atexit(stop_workers_at_exit);

    if (dd >= 0)
        parse_args(argc - dd - 1, argv + dd + 1);
    else
        parse_args(0, NULL);

    if (rte_eal_has_hugepages() == 0)
        fprintf(stderr, "提示: 当前没有使用大页（--no-huge），mlx5 需要大页，"
                        "本模式下只适合 pcap/虚拟设备自测。\n");

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* 端口选择 */
    uint16_t port = 0;

    if (g_pci[0] != '\0') {
        if (rte_eth_dev_get_port_by_name(g_pci, &port) != 0)
            rte_exit(EXIT_FAILURE,
                     "找不到设备 %s。先用 `dpdk-devbind.py -s` 看 PCI 地址，"
                     "并确认它出现在 --allow 列表里。\n", g_pci);
    } else if (rte_eth_dev_count_avail() == 0) {
        rte_exit(EXIT_FAILURE, "没有可用 DPDK 端口（检查 --allow/-a 参数）\n");
    }

    g_pool = rte_pktmbuf_pool_create("pktloss_pool", NB_MBUF, MBUF_CACHE, 0,
                                     MBUF_DATA_ROOM, SOCKET_ID_ANY);
    if (g_pool == NULL)
        rte_exit(EXIT_FAILURE,
                 "mbuf 池创建失败（需要 %u 个 %u B 的 mbuf，约 %u MiB）。"
                 "大页不够就加大 setup.sh 里的页数。\n",
                 NB_MBUF, MBUF_DATA_ROOM,
                 (unsigned)((uint64_t)NB_MBUF * (MBUF_DATA_ROOM + 256) / (1024 * 1024)));

    /* 分配 worker：主 lcore 只做统计与信号；每个收包队列一个 worker。 */
    /* 顺序有意为之：先让 worker 在 lcore 上就绪并等 g_rx_enabled，再
     * configure/start 端口并建 flow，最后放行 —— 把 start 到有人收包
     * 的窗口压到微秒级。 */
    const unsigned main_lcore = rte_get_main_lcore();
    const unsigned nworker    = g_soft_steer ? 1u : (unsigned)g_nchan;
    unsigned       lcores[MAX_CHAN];
    unsigned       nlcore = 0;

    for (unsigned l = 0; l < RTE_MAX_LCORE && nlcore < nworker; l++) {
        if (l == main_lcore || rte_lcore_is_enabled(l) == 0)
            continue;
        lcores[nlcore++] = l;
    }

    if (nlcore < nworker) {
        rte_exit(EXIT_FAILURE,
                 "需要 %u 个 worker lcore（加 1 个主 lcore），当前只找到 %u 个。"
                 "用 -l 0-%u 给足 lcore。\n",
                 nworker, nlcore, nworker);
    }

    static struct worker_arg wargs[MAX_CHAN];

    for (unsigned w = 0; w < nworker; w++) {
        wargs[w].port  = port;
        wargs[w].queue = (uint16_t)w;
        wargs[w].soft  = g_soft_steer;
        wargs[w].other = 0;

        if (rte_eal_remote_launch(worker_main, &wargs[w], lcores[w]) != 0)
            rte_exit(EXIT_FAILURE, "在 lcore %u 上启动 worker 失败\n", lcores[w]);
    }

    if (port_init(port) != 0)
        rte_exit(EXIT_FAILURE, "端口初始化失败\n");

    if (g_show_xstats) {
        printf("\n== 端口 %u 全部计数器 ==\n", port);
        dump_all_xstats(port);
        rte_eth_dev_stop(port);
        rte_eth_dev_close(port);
        rte_eal_cleanup();
        return 0;
    }

    init_xstat_idx(port);

    /* 中途接入时，rte_eth_dev_start 到 flow 规则建好之间（实测 ~0.26 s）的包
     * 走默认 RSS，队列/通道映射是乱的；worker 又被 g_rx_enabled 挡着，8192
     * 深的队列会灌满溢出（实测第一秒 out_of_buffer +13 万）。放行 worker 之前
     * 先排空队列，把这段窗口的残留丢掉：worker 看到的第一个包是 flow 生效之后
     * 的包，编号连续，不会在 ch0 上记成一次几十 k 的假跳跃。
     * 这批包确实丢了（启动窗口内本来就收不到），但不污染稳态丢包率。 */
    /* --no-drain 时必须跳过：pcap PMD 在 dev_start 后一次性灌完再也没数据，
     * 排空等于把待测的包全扔了。 */
    if (g_drain_start) {
        struct rte_mbuf *dbuf[BURST];
        const uint16_t   nq  = g_soft_steer ? 1u : (uint16_t)g_nchan;
        unsigned         drop = 0;

        for (uint16_t q = 0; q < nq; q++) {
            /* 上限防活锁：drain 速度远高于 156 kpps/队列，正常情况下一次就空 */
            unsigned budget = RX_DESC * 4u;
            uint16_t dn;

            while (budget > 0 &&
                   (dn = rte_eth_rx_burst(port, q, dbuf, BURST)) > 0) {
                rte_pktmbuf_free_bulk(dbuf, dn);
                drop   += dn;
                budget -= dn;
            }
        }
        if (drop > 0)
            printf("启动窗口积压的 %u 个包（flow 生效前）已丢弃，"
                   "统计从之后的第一个包开始\n", drop);
    }

    atomic_store_explicit(&g_t0_ns, now_mono_ns(), memory_order_relaxed);
    atomic_store_explicit(&g_rx_enabled, 1, memory_order_release);

    if (g_csv_path[0] != '\0') {
        g_csv = fopen(g_csv_path, "a");
        if (g_csv == NULL) {
            fprintf(stderr, "无法打开 CSV %s: %s\n", g_csv_path, strerror(errno));
        } else {
            fprintf(g_csv,
                    "unix_time,elapsed_s,chan,recv,pps,lost,gaps,maxgap,"
                    "rate_ub,rounds,dup,bad,mbuf_avail\n");
        }
    }

    printf("pktloss: %s，端口 %u，%d 路（ch %s），%s，%u 个 worker\n",
           rte_version(), port, g_nchan, fmt_chans(),
           g_soft_steer ? "单队列软分流" : "每通道一队列", nworker);
    if (g_duration > 0.0)
        printf("计划运行 %.1f s；Ctrl-C 可提前结束并打印汇总。\n", g_duration);
    else
        printf("一直运行，Ctrl-C 结束并打印汇总。\n");

    const double t0    = now_mono();

    g_port   = port;
    g_prev_t = 0.0;

    /* 启动窗口（flow 生效前）的溢出计数留在硬件里，基线从这里开始算增量 */
    snapshot_port_baseline();

    double t_next = g_interval;

    for (;;) {
        struct timespec req = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000 };

        nanosleep(&req, NULL);

        const double el = now_mono() - t0;

        if (el >= t_next) {
            print_tick(el);
            t_next += g_interval;
        }
        if (atomic_load(&g_stop))
            break;
        if (g_duration > 0.0 && el >= g_duration)
            break;
    }

    atomic_store(&g_stop, 1);

    /* 等 worker 退出，再关设备 —— 顺序反了会 segmentation fault。 */
    unsigned launched = 0;

    for (unsigned l = 0; l < RTE_MAX_LCORE; l++) {
        if (rte_lcore_is_enabled(l) && l != main_lcore)
            launched++;
    }
    for (unsigned l = 0; l < RTE_MAX_LCORE; l++) {
        if (rte_lcore_is_enabled(l) && l != main_lcore)
            (void)rte_eal_wait_lcore(l);
    }

    const double elapsed = now_mono() - t0;

    if (elapsed - g_prev_t > 0.05)      /* 避免和最后一次 tick 重复 */
        print_tick(elapsed);

    print_summary(elapsed, port);

    uint64_t lost_total = 0;

    for (int c = 0; c < g_nchan; c++)
        lost_total += atomic_load(&chans[c].lost);

    print_port_stats(port);
    printf("\n");
    print_verdict(port, lost_total);

    if (g_csv) {
        fclose(g_csv);
        g_csv = NULL;
    }
    if (g_raw_fp) {
        fclose(g_raw_fp);
        g_raw_fp = NULL;
    }

    struct rte_flow_error err;

    (void)rte_flow_flush(port, &err);
    rte_eth_dev_stop(port);
    rte_eth_dev_close(port);
    rte_eal_cleanup();
    return 0;
}
