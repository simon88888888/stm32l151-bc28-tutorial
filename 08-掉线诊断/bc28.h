/**
  ******************************************************************************
  * @file    HARDWARE/bc28/bc28.h
  * @brief   BC28 (NB-IoT) 驱动 —— 自己写的, 不搬厂家 HARDWARE/BC28。
  *
  *  配套手册: 第 3 篇 · 把数据发到自己的服务器
  *
  *  跟厂家 BC28.c 的三处根本差别(逐条对照见第 3 篇 §3):
  *
  *    1) 建连的判据是 "+NSOCO: <n>" 这条 URC, 不是 AT 回的 OK。
  *       厂家 BC28_ConTCP() 整个文件里根本没出现过 "+NSOCO" 这个串。
  *       -> 见 BC28_SKIP_NSOCO_URC, 留了 A/B 开关把厂家那种写法复现出来。
  *
  *    2) 收数走自己的线性缓冲 + 轮询 USART2->DR, 不用 usart.c 的 buf_uart2。
  *       那个是 256 字节的线性缓冲, 满了把 index 归零(静默回绕), 等 URC 的
  *       22 秒里会被覆盖 8 次。
  *
  *    3) 所有超时都是"有上限的等待"。厂家是每种状态循环 100 次 x 300ms = 30 秒,
  *       而且循环里既不重发也不清缓冲。
  ******************************************************************************
  */

#ifndef __BC28_H
#define __BC28_H

#include "stm32l1xx.h"

/* ---- 返回值 -------------------------------------------------------------
   >= 0 成功(值有意义时才 > 0); < 0 是下面这些错误码。 */
#define BC28_E_ARG      (-1)    /* 调用方参数不对 */
#define BC28_E_TIMEOUT  (-2)    /* 等超时 */
#define BC28_E_ERROR    (-3)    /* 模块回了 ERROR */
#define BC28_E_NOSOCK   (-4)    /* NSOCR 没分配到 socket */
#define BC28_E_PARSE    (-5)    /* 回了 OK, 但内容解析不出来 */
#define BC28_E_NORESP   (-6)    /* 等超时, 而且**模块一个字都没回** —— 见 BC28_WaitNet */

/* ---- A/B 对照开关(只影响 BC28_Connect 那一步) ---------------------------
 *
 *   0 = 正常写法: 发完 AT+NSOCO 必须等到 "+NSOCO: <n>" 这条 URC 才算连上。
 *   1 = 复现厂家的写法: 收到 OK 就当连上了, 立刻发数据。
 *       —— 厂家 BC28_ConTCP() 里只等 "OK", 文件里没有 "+NSOCO"。
 *
 *   第 3 篇 §6 的对照实验就是改这一个数、重编译、重烧。
 *   也可以从 Keil 界面加, 不用改这个文件:
 *     Options for Target -> C/C++ -> Define:  BC28_SKIP_NSOCO_URC=1 */
#ifndef BC28_SKIP_NSOCO_URC
#define BC28_SKIP_NSOCO_URC     0
#endif

/* 上面那一档"假装连上了"之后硬等多久(ms), 用来模拟厂家那种死等。
   0 = 一点都不等, 立刻发 —— 这样差别最干净, 一眼看得出是"等没等 URC"。 */
#ifndef BC28_SKIP_WAIT_MS
#define BC28_SKIP_WAIT_MS       0
#endif

/* ---- 超时取值(单位 ms), 理由见第 3 篇 §4 -------------------------------
   每个数都是"实测值 + 余量", 不是拍脑袋:
   普通 AT 1000 / 要翻一翻的 2000 / NUESTATS 是本模块回得最慢的一条 5000 /
   +NSOCO URC 实测 2~22 秒所以给 30000。

   ★ 注网这一档 2026-09-16 从 120000 提到 300000 —— **原来的 120 秒正好卡在
     实测值的边界上, 把板子彻底卡死了**。实测: `AT+NRB` 之后模块要从头搜网
     **约 2 分钟**才 CEREG=1/5(23:22 那次是 121 秒), 而 120 秒一到 main.c 就
     又发 AT+NRB → 模块刚搜到一半被重启 → 再搜 → 再超时 …… **永远搜不完**。
     串口指纹: 一直 `CEREG stat=2`(搜网中) + CSQ 25~26(信号好得很) + 每 120 秒
     一条 `! network attach timeout` 跟着一条 `! rebooting BC28`。
     提到 300000 是给实测 2 分钟留 2.5 倍余量。 */
#define BC28_TO_AT      1000
#define BC28_TO_LONG    2000
#define BC28_TO_STATS   5000
#define BC28_TO_NSORF   5000
/* ★ 2026-09-20: 30000 -> 60000。

   原来 30 秒是照着"ECL=2 时实测建连 25551 ms"定的（第 7 篇 §9 记的就是它,
   当时只剩 4.5 秒余量, 但**故意没动**）。整夜实测给了新的理由:

   2026-09-19 那一夜 170 次建连失败里 **114 次是 AT+NSOCO 卡满 30 秒** ——
   也就是说这个 30 秒的上限**天天在切**。而成功建连的中位耗时只有 3 秒、
   最慢 16 秒。那 114 次不是"慢", 是**另一个世界**: 模块压根没回 URC。

   放宽到 60000 不是"修好了", 是**把上限抬到能分辨"真慢"和"真死"**的地方:
   如果放宽之后有一部分能连上, 说明 30 秒原本切早了; 如果还是全卡满,
   那就证明它们是真的没回应 —— 而那种情况该由 connect_failed() 那边
   **只重试、不 NRB** 来兜（见 main.c 里 connect_failed() 开头那段）。

   代价: 一次真死的建连从 30 秒变 60 秒才认输。但那条路**已经不再触发
   NRB** 了, 所以这个代价只是"这一轮慢一点", 不会再放大成"离线 2~8 分钟"。 */
#define BC28_TO_URC    60000
#define BC28_TO_NET   300000

/* ★ 关 socket 的两档 (2026-09-17 晚加) —— 这是 socket 泄漏的**根因**所在。

   原来 BC28_Close() 用的是 BC28_TO_LONG, **只等 2 秒**; 而 BC28_Connect()
   用的是 BC28_TO_URC, 等 30 秒。也就是说: 一个刚花了 30 秒还没连上的 socket,
   模块正忙着在 TCP 那层重传, 却被要求 2 秒内回 AT+NSOCL —— 必然超时。
   超时就不算关掉, 号没还回去, **漏一个**。模块一共只有 6 个。

   实测指纹 (2026-09-17 23:38:30~23:41:15, 网络很差的那 5 分钟):
       [sock] open=1 close=1 balance=0     ← 网络好时, 2 秒够, 不漏
       [sock] open=2 close=1 balance=1     ← 一开始超时
       [sock] open=5 close=1 balance=4     ← 5 轮开 5 个, 只关掉 1 个
   每次 NSOCO 超时必跟着一次 NSOCL 失败 —— 相关性 100%, 因为两者是同一件事。

   所以关 socket 要给**和建连同量级的耐心**, 而且超时要重试。10 秒 × 3 次,
   最坏 ~32 秒; 正常路径(模块秒答 OK)一点不花。 */
#define BC28_TO_CLOSE  10000
#define BC28_CLOSE_TRY 3

/* 一次能发的最大字节数。要发更长就多调几次 BC28_Send。
   (AT+NSOSD 那一行最长是这个数的 2 倍 + 命令头, 见 bc28.c 里的 cmd 缓冲) */
#define BC28_TXMAX      192

/* ---- 无线信息 -----------------------------------------------------------
   注意: 模块给的 dB 字段**单位是 0.1**, 这里原样存(例如 -683), 换算交给用的人。
   第 2 篇 §4.4 已经定过, 第 3 篇 §4 再强调一次。 */
typedef struct
{
    int csq;        /* AT+CSQ 的 0~31, 99 = 未知。换算 dBm 约 -113 + 2*csq */
    int rsrp;       /* Signal power, 0.1 dBm */
    int snr;        /* SNR, 0.1 dB */
    int ecl;        /* 覆盖增强等级 0~2, 越大说明信号越差 */
    int band;       /* 当前频段 */

    /* ---- 当前小区 (2026-09-19 加) ----
       用途只有一个: 看清"夜里那几次掉线是不是都在跳小区"。
       **不是用来定位的** —— AT+QLBS 这版固件不支持, 而拿小区号去查公开众包库
       实测查不到(免费那家直接 404), 详见第 6 篇 / 看板底部说明。

       ★ 两个都**没读上来时是 -1, 不是 0** —— 0 是个合法的小区号(虽然实际几乎
         不会出现), 拿 0 当"没读到"会让报文里分不清"真读到 0"和"根本没读到"。
         main.c 那边的规矩是: 只有 cell_id >= 0 才往报文里塞 ,CID=,PCI=。 */
    int cell_id;    /* NUESTATS 的 Cell ID, 28 位 */
    int pci;        /* NUESTATS 的 PCI, 物理小区号 0~503 */
} BC28_RADIO;

/* ---- 底层 --------------------------------------------------------------- */
void     BC28_Init(void);              /* USART2 + 时基 + 关掉 RXNE 中断 + 重启模块 */
void     BC28_Reset(void);             /* AT+NRB 让模块整个重启(救 socket 卡死) */
void     BC28_Idle(uint32_t ms);       /* 空转 ms 毫秒, 一边收一边走时钟 */
uint32_t BC28_Millis(void);            /* 精确毫秒时基, 不丢 tick */

/* ---- STOP 低功耗 --------------------------------------------------------
 *
 *   BC28_SleepMs() 是省电功能**唯一的入口**: 进 STOP 睡 ms 毫秒, 醒来自己把
 *   时钟(32MHz)、时基(SysTick + TIM4)、UART 残留、模块探活全做完再返回。
 *
 *   ★ 底线: **RTC 唤醒源起不来就绝不进 STOP**, 自动退回 BC28_Idle 空转。
 *     宁可费电, 不能睡死 —— 睡死的板子要人去现场拔电。
 *
 *   ★ 只在"两轮上报之间"那段长等待里调它。建连/收数中间**不要**睡:
 *     那时候正要跟模块对话, 睡下去必丢字节。 */

#define BC28_SLEEP_MIN_MS   1000UL     /* 比这还短就不值得睡, 直接空转 */

int      BC28_SleepMs(uint32_t ms);    /* 返回实际睡掉的毫秒(已补进 BC28_Millis) */
void     BC28_MillisAdd(uint32_t ms);  /* 手动把睡掉的时基补回来 */
void     BC28_RxFlush(void);           /* 丢掉 STOP 期间卡在 USART2 里的半截字节 */

/* 0 = 唤醒源不可用(不会进 STOP) / 1 = LSE / 2 = LSI。开机务必打印 ——
   用 LSI 时时间基准本身就不准, 服务端看到的 UP= 会跟着漂。 */
uint8_t  BC28_RtcOk(void);
uint32_t BC28_SleepCnt(void);          /* 进过几次 STOP(诊断) */
uint32_t BC28_SleepMsSum(void);        /* 累计睡掉多少毫秒(诊断) */

/* ★ 清 WUTF + EXTI 线 20 的挂起位。**给 stm32l1xx_it.c 的 RTC_WKUP_IRQHandler
   用的** —— 那边是唤醒的最前端, 不清这两下, 轻则"睡了等于没睡", 重则
   EXTI 上再也形不成上升沿、**醒不过来**。放这儿是因为要显式解 WPR, 见实现里的注释。 */
void     BC28_WutClear(void);

/* ---- 模块 --------------------------------------------------------------- */
int  BC28_Probe(char *info, uint16_t cap);     /* AT -> ATE0 -> ATI, info 收 ATI 回显 */
int  BC28_WaitNet(uint32_t to_ms);             /* 轮询 AT+CEREG? 直到注网, 0 = 已注网 */
int  BC28_GetCsq(void);                        /* >= 0 = csq, < 0 = 错误码 */
int  BC28_GetRadio(BC28_RADIO *r);             /* AT+NUESTATS + AT+CSQ */

/* ---- socket ------------------------------------------------------------- */
int  BC28_Resolve(const char *host, char *ip_out, uint16_t cap);  /* 域名 -> IP, 用模块自带的 DNS */
int  BC28_Open(void);                                          /* >= 0 = socket 号 */
int  BC28_Connect(int sock, const char *ip, uint16_t port, uint32_t to_ms);
int  BC28_Send(int sock, const char *buf, uint16_t len);
int  BC28_Recv(int sock, char *out, uint16_t cap, uint32_t to_ms);  /* >= 0 = 收到的字节数 */
int  BC28_Close(int sock);

/* socket 账本: 开/关各数了多少次, 差额 = 怀疑漏掉的个数。
   模块一共只有 6 个 socket, 漏光就 NSOCR 回 ERROR、NSOCL 也关不掉, 只能 NRB。
   差额 > 0 就是漏了 —— 而且**在漏光之前**就能看见 (见 bc28.c 里 sock_open_n 那段)。
   BC28_Reset() 会把两个数清零, 因为 NRB 会放掉模块那边的全部 socket。 */
uint32_t BC28_SockOpenCnt(void);
uint32_t BC28_SockClosedCnt(void);
int      BC28_SockBalance(void);
/* +NSOCLI 带回来的 socket 号 —— "我们以为关了" 和 "模块真关了" 是不是同一时刻,
   全靠这两个数对照 (见 bc28.c 里 sock_cli_last 那段)。 */
int      BC28_SockCliLast(void);
uint32_t BC28_SockCliCnt(void);

/* ---- 诊断 --------------------------------------------------------------- */
uint8_t     BC28_SocketClosed(void);   /* 收到过 +NSOCLI(运营商把 socket 掐了) */
uint8_t     BC28_RxOverflow(void);     /* 收缓冲满过, 有数据被丢 */
uint32_t    BC28_NsocoDelayMs(void);   /* 上次 NSOCO 后, +NSOCO URC 隔了多久才来 */
const char *BC28_Why(int rc);          /* 错误码 -> 可以直接打日志的短串 */
void        BC28_DumpRx(void);         /* 把 RX 缓冲里的残留打到 USART1(诊断用) */

#endif /* __BC28_H */
