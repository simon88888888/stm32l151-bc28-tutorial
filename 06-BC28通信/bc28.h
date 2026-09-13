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
   +NSOCO URC 实测 2~22 秒所以给 30000 / 注网要冷启动找小区给 120000。 */
#define BC28_TO_AT      1000
#define BC28_TO_LONG    2000
#define BC28_TO_STATS   5000
#define BC28_TO_NSORF   5000
#define BC28_TO_URC    30000
#define BC28_TO_NET   120000

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
} BC28_RADIO;

/* ---- 底层 --------------------------------------------------------------- */
void     BC28_Init(void);              /* USART2 + 时基 + 关掉 RXNE 中断 */
void     BC28_Idle(uint32_t ms);       /* 空转 ms 毫秒, 一边收一边走时钟 */
uint32_t BC28_Millis(void);            /* 精确毫秒时基, 不丢 tick */

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

/* ---- 诊断 --------------------------------------------------------------- */
uint8_t     BC28_SocketClosed(void);   /* 收到过 +NSOCLI(运营商把 socket 掐了) */
uint8_t     BC28_RxOverflow(void);     /* 收缓冲满过, 有数据被丢 */
uint32_t    BC28_NsocoDelayMs(void);   /* 上次 NSOCO 后, +NSOCO URC 隔了多久才来 */
const char *BC28_Why(int rc);          /* 错误码 -> 可以直接打日志的短串 */
void        BC28_DumpRx(void);         /* 把 RX 缓冲里的残留打到 USART1(诊断用) */

#endif /* __BC28_H */
