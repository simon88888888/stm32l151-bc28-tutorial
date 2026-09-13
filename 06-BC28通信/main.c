/**
  ******************************************************************************
  * @file    Project/Test/main.c
  * @brief   第 3 篇 · 把数据发到自己的服务器
  *
  *  上电 -> 注网 -> 建 socket -> 每 30 秒把一行信号数据发到自己的服务器。
  *
  *  跟第 2 篇的分工:
  *    第 2 篇是**人在 PC 上敲 AT**, 证明"这条链路能通";
  *    这一篇把那个过程**搬进固件**, 让它自己跑 —— 不用 PC, 也不用有人看着。
  *
  *  三个开关, 改完重编译重烧:
  *    MODE_LONG            1 = 长连接(默认) / 0 = 每轮重连, 用来量建连耗时
  *    BC28_SKIP_NSOCO_URC  在 bc28.h, =1 就是复现厂家那种"不等 URC 就发"
  *    REPORT_MS            上报间隔
  *
  *  串口: USART1 -> CH340 -> 电脑, 9600 8-N-1, 流控无
  *        (第 1 篇那套 flash_and_run.ps1 / read_com.ps1 直接就能看)
  *
  *  ⚠ **日志一律用英文。** AC5 在字符串字面量里遇到中文会报
  *    warning #870-D: invalid multibyte character sequence ——
  *    注释里可以写中文(注释在编译前就被剥掉了), 打进串口的字符串不行。
  ******************************************************************************
  */

/* ---- 实验二 的 A/B 开关 ---------------------------------------------------
   ⚠ 必须定义在 `#include "bc28.h"` **之前** —— bc28.h 里那一段是 #ifndef 保护的,
     先定义才能盖住它的默认值 0。

   0 = 正常写法: 发完 AT+NSOCO 之后必须等到 "+NSOCO: <n>" 这条 URC 才算连上。
   1 = 复现**厂家**的写法: 收到 OK 就当成连上了, 立刻发数据。
       (厂家 BC28_ConTCP() 整个文件里没出现过 "+NSOCO" 这个串。)

   第 3 篇 §6 的对照实验就是只改这一个数、重编译、重烧 —— 同一份固件、
   同一个服务器、同一个时间段, 只差"等没等那条 URC"。 */
#define BC28_SKIP_NSOCO_URC     0

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usart.h"
#include "led.h"
#include "bc28.h"
#include <stdio.h>
#include <string.h>

/* ---- 你的服务器 -----------------------------------------------------------
   **写域名比写 IP 好。** 家宽的出口 IP 是动态的, 光猫重启一次就变; 写死 IP
   意味着每次变了都要重编译重烧 —— 那就等于没法长期挂着。

   固件自己判断这一行是哪种:
     - 看着像 IPv4(四段数字) -> 直接用, 一次 AT 都不多发
     - 否则当域名           -> 先用**模块自带的 AT+QDNS** 解析成 IP 再连

   好消息是这件事不用自己做: 模块自带 DNS 解析, 所以
     - 固件里不需要塞 DNS 客户端(几百行, 还要自己处理 UDP 重传)
     - 不需要给模块开 IPv6(移动 NB-IoT 本来也没给模块分 v6)
     - 不需要额外挂 DDNS 客户端, 只要域名那边有个**带 A 记录**的 DDNS 就行

   ⚠ 公开仓库里这一行必须是占位符 —— 别把自己的服务器地址提交上去。
     真值只留在本机这份工程里。 */
#define SERVER_HOST     "your.domain.here"    /* 或直接写 "1.2.3.4" */
#define SERVER_PORT     9101

/* 预共享 token。**真值只在服务器的 sink.env(600) 里, 不进任何仓库。**
   服务端每条连接的第一行必须是 "#TOKEN <它>", 不对就记一条 REJECT 然后断开。
   为什么公网裸端口一定要有这个 —— 见第 3 篇 §2。 */
#define SINK_TOKEN      "put-your-32-hex-token-here"

/* ---- 开关 -----------------------------------------------------------------*/
#define MODE_LONG       1               /* 1 = 长连接(推荐); 0 = 每轮建连->发->收->关 */
#define REPORT_MS       30000UL         /* 上报间隔 */

#if MODE_LONG
#define MODE_NAME       "long"
#else
#define MODE_NAME       "short"
#endif

/* Private variables ---------------------------------------------------------*/
static __IO uint32_t TimingDelay;

static int      sock = -1;      /* 当前 socket, -1 = 没连上 */
static uint32_t seq;            /* 上报序号, 从 1 开始 */
static uint32_t rx_ok;          /* 服务器回了 OK 的次数 */
static uint32_t rx_lost;        /* 发出去了但没等到 OK 的次数 */

#if !MODE_LONG
/* 短连接模式的建连耗时统计(长连接那一档用不到, 不声明就不会有 unused 警告) */
static uint32_t conn_n, conn_min, conn_max, conn_sum;
#endif

/* Private function prototypes -----------------------------------------------*/
void Delay(__IO uint32_t nTime);
void TimingDelay_Decrement(void);

static int  put_tenth(char *p, int v);
static void trim(char *s);
static int  wait_reply(int s, char *out, uint16_t cap, int rounds);
static int  link_up(void);
static void report_once(void);

/**
  * @brief  把一个"单位 0.1"的整数打成人看的样子: -683 -> "-68.3", 64 -> "6.4"。
  *         AT+NUESTATS 的 dB 类字段单位就是 0.1(第 2 篇 §4.4), 别当整数用。
  */
static int put_tenth(char *p, int v)
{
    char *s = p;

    if (v < 0)
    {
        *p++ = '-';
        v = -v;
    }
    p += sprintf(p, "%d.%d", v / 10, v % 10);
    return (int)(p - s);
}

/** @brief  去掉行尾的 \r\n, 免得打日志时把版面拆了。 */
static void trim(char *s)
{
    size_t n = strlen(s);

    while ((n > 0) && ((s[n - 1] == '\r') || (s[n - 1] == '\n') || (s[n - 1] == ' ')))
    {
        s[--n] = '\0';
    }
}

/**
  * @brief  看着像 IPv4 吗?  "1.2.3.4" 是, "example.com" 不是。
  *         只判形状(四段十进制数), **不校验数值范围** —— 够用, 也省得引 inet_aton
  *         那套要额外缓冲的东西。判错了也不危险: 顶多多走一次 DNS。
  */
static int is_ipv4(const char *s)
{
    int part = 0;
    int digits = 0;

    if (s == NULL)
    {
        return 0;
    }
    for (; *s != '\0'; s++)
    {
        if ((*s >= '0') && (*s <= '9'))
        {
            digits++;
            if (digits > 3)
            {
                return 0;               /* 一段超过 3 位数字, 不是 */
            }
        }
        else if (*s == '.')
        {
            if (digits == 0)
            {
                return 0;               /* 空段, 像 "1..2" */
            }
            part++;
            digits = 0;
            if (part > 3)
            {
                return 0;
            }
        }
        else
        {
            return 0;                   /* 出现字母/连字符 -> 是域名 */
        }
    }
    return ((part == 3) && (digits > 0));
}

/**
  * @brief  得到真正要连的 IP 字符串: 写的是 IP 就原样用, 写的是域名就先解析。
  *         返回 0 成功, < 0 = BC28_Resolve 的错误码。
  */
static int resolve_target(char *out, uint16_t cap)
{
    char buf[64];
    int  rc;

    if ((out == NULL) || (cap < 8))
    {
        return BC28_E_ARG;
    }

    if (is_ipv4(SERVER_HOST))
    {
        /* 写的就是 IP —— 省掉一次 AT 往返。这一支就是没上 DDNS 时的样子。 */
        strncpy(out, SERVER_HOST, (size_t)(cap - 1));
        out[cap - 1] = '\0';
        return 0;
    }

    rc = BC28_Resolve(SERVER_HOST, buf, sizeof(buf));
    if (rc < 0)
    {
        printf("! DNS: cannot resolve %s: %s\r\n", SERVER_HOST, BC28_Why(rc));
        return rc;
    }
    printf("+ DNS: %s -> %s   (module's own AT+QDNS)\r\n", SERVER_HOST, buf);

    strncpy(out, buf, (size_t)(cap - 1));
    out[cap - 1] = '\0';
    return 0;
}

/**
  * @brief  读服务端回的一行(OK <n> / AUTH OK)。
  *         AT+NSORF 要**轮询**: 数据什么时候到是另一回事, 一条读不到就再读。
  *         返回字节数, 0 = 这几轮都没有。
  */
static int wait_reply(int s, char *out, uint16_t cap, int rounds)
{
    int i;
    int n;

    for (i = 0; i < rounds; i++)
    {
        n = BC28_Recv(s, out, cap, BC28_TO_NSORF);
        if (n > 0)
        {
            trim(out);
            return n;
        }
        if ((n < 0) && (n != BC28_E_TIMEOUT))
        {
            return n;               /* 真错误, 不装了 */
        }
        BC28_Idle(1000);
    }
    return 0;
}

/**
  * @brief  建一条到服务器的连接, 并把 token 交上去。
  *         返回 socket 号(>= 0), 或负错误码。
  */
static int link_up(void)
{
    char buf[64];
    char ip[64];
    int  s;
    int  rc;

    s = BC28_Open();
    if (s < 0)
    {
        printf("! NSOCR failed: %s\r\n", BC28_Why(s));
        return s;
    }

    /* 每次建连前都重新解析一次 —— 这正是"写域名"的价值所在:
       家里出口 IP 变了, 下一次重连自然就拿到新的, 固件一个字节都不用动。
       (所以解析放在 link_up 里、而不是开机时算一次存起来。) */
    rc = resolve_target(ip, sizeof(ip));
    if (rc < 0)
    {
        (void)BC28_Close(s);
        return rc;
    }

    rc = BC28_Connect(s, ip, SERVER_PORT, BC28_TO_URC);
    if (rc < 0)
    {
        printf("! NSOCO: no +NSOCO URC: %s\r\n", BC28_Why(rc));
        BC28_DumpRx();
        (void)BC28_Close(s);
        return rc;
    }

#if (BC28_SKIP_NSOCO_URC == 0)
    printf("+ TCP connected %s:%u  socket=%d  (+NSOCO URC came after %lu ms)\r\n",
           ip, (unsigned)SERVER_PORT, s,
           (unsigned long)BC28_NsocoDelayMs());
#else
    /* 这一档不等 URC, 所以此刻**根本不知道有没有真连上** —— 这正是要害 */
    printf("+ NSOCO got OK, assuming connected (socket=%d) -- vendor style\r\n", s);
#endif

    /* 握手: 每条连接的第一行必须是 token。
       ⚠ 行尾的 \r\n **不能省**。服务端是 readline() 按行读的, 少这一个换行,
       它会一直阻塞等下去, 从外面看就像"数据根本没到" —— 我们在第 3 篇实测里
       真踩了这个坑: 抓包显示每个字节都到了、内核也 ACK 了, 但服务端一条
       日志都没有。定位花了很久, 因为现象跟"链路不通"一模一样。 */
    rc = BC28_Send(s, "#TOKEN " SINK_TOKEN "\r\n",
                   (uint16_t)strlen("#TOKEN " SINK_TOKEN "\r\n"));
    if (rc < 0)
    {
        printf("! send token failed: %s\r\n", BC28_Why(rc));
        return rc;
    }

    /* 等不到 AUTH OK 也**不算连接失败**: 照发不误。
       理由有两个 —— 一是 NB-IoT 这条链路本来就慢, 二是 A/B 对照那一档
       故意不等, 如果这里一失败就重连, 就永远走不到"发数据"那一步,
       也就看不到"服务器到底收到什么"这个结果了。 */
    if (wait_reply(s, buf, sizeof(buf), 2) > 0)
    {
        printf("+ server: %s\r\n", buf);
    }
    else
    {
        printf("! no AUTH OK (noted, going on anyway)\r\n");
    }
    return s;
}

/**
  * @brief  发一条上报, 并读服务端的应答。
  *         一行的样子: #12,CSQ=28,RSRP=-68.3,SNR=6.4,ECL=0,UP=360
  */
static void report_once(void)
{
    BC28_RADIO r;
    char       body[128];
    char       buf[64];
    char      *p;
    int        rc;
    int        n;
    int        i;

    memset(&r, 0, sizeof(r));
    if (BC28_GetRadio(&r) < 0)
    {
        r.csq = -1;
        printf("! radio read failed (sending anyway, signal fields = 0)\r\n");
    }

    p  = body;
    p += sprintf(p, "#%lu,CSQ=%d,RSRP=", (unsigned long)seq, r.csq);
    p += put_tenth(p, r.rsrp);
    p += sprintf(p, ",SNR=");
    p += put_tenth(p, r.snr);
    p += sprintf(p, ",ECL=%d,UP=%lu", r.ecl,
                 (unsigned long)(BC28_Millis() / 1000));
    p += sprintf(p, "\r\n");            /* 行尾换行, 理由见 link_up() 里那段注释 */

    rc = BC28_Send(sock, body, (uint16_t)strlen(body));
    if (rc < 0)
    {
        printf("[%lu] send failed: %s\r\n", (unsigned long)seq, BC28_Why(rc));
        BC28_DumpRx();
        rx_lost++;
        return;
    }

    /* 读应答。**这里要跳过迟到的 "AUTH OK"** —— 那是握手那一行的回执。
       link_up() 等它只等 2 轮, 等不到就放过去了(不作为失败), 于是它常常在
       下面这一两轮才浮上来。不跳过的话它会被当成"这一条上报的 ACK", 于是
       lost 计数**虚高**: 实测里串口报 lost 3, 而服务端日志显示那 3 条一条不少
       全到了 —— 是判据错, 不是链路错。这条记在第 3 篇 §8。 */
    for (i = 0; i < 3; i++)
    {
        n = wait_reply(sock, buf, sizeof(buf), 3);
        if ((n > 0) && (strcmp(buf, "AUTH OK") == 0))
        {
            printf("  (late) server: %s -- that was the handshake, not this one\r\n", buf);
            continue;
        }
        break;
    }

    if (n > 0)
    {
        rx_ok++;
        printf("[%lu] %s  <- %s   (server OK %lu, lost %lu)\r\n",
               (unsigned long)seq, body, buf,
               (unsigned long)rx_ok, (unsigned long)rx_lost);
    }
    else
    {
        rx_lost++;
        printf("[%lu] %s  <- no reply   (server OK %lu, lost %lu)\r\n",
               (unsigned long)seq, body,
               (unsigned long)rx_ok, (unsigned long)rx_lost);
    }
}

/**
  * @brief  Main program.
  */
int main(void)
{
    char     info[128];
    uint32_t cal;
    int      rc;
    int      i;

    /* 1ms SysTick 中断(TimingDelay_Decrement 由它调, 不能删) */
    if (SysTick_Config(SystemCoreClock / 1000))
    {
        while (1);
    }

    LED_Init();
    uart1_init(9600);   /* USART1 -> CH340 -> 电脑: printf 就是走这条路出去的。
                           漏了这一句的现象很典型: 烧录成功、板子在跑, 但串口
                           一个字都没有 —— 因为 USART1 的时钟和引脚都还没配。 */
    BC28_Init();        /* 里面会 uart2_init(9600) */

    printf("\r\n\r\n");
    printf("### Part 3: uplink to my own server ###\r\n");
    printf("build:  " __DATE__ " " __TIME__ "\r\n");
    printf("server: %s:%u   mode: %s   report: %lu ms\r\n",
           SERVER_HOST, (unsigned)SERVER_PORT, MODE_NAME, (unsigned long)REPORT_MS);
    printf("target is %s -- %s\r\n",
           is_ipv4(SERVER_HOST) ? "an IP" : "a hostname",
           is_ipv4(SERVER_HOST) ? "connecting directly"
                                : "will resolve with AT+QDNS on every connect");
#if (BC28_SKIP_NSOCO_URC != 0)
    printf("!! A/B build BC28_SKIP_NSOCO_URC=1: send right after OK,");
    printf(" do NOT wait for +NSOCO URC\r\n");
#endif
    printf("\r\n");

    /* ---- ⓪ 时基自检 ----
       Delay(1000) 是 SysTick 中断驱动的(第 4 篇拿它量过墨水屏刷新, 数值对得上
       数据手册), 所以它可以当尺子。拿它量 TIM4 那套毫秒时基 ——
       这一句能一眼抓住"尺子错了"那类 bug。第一版我就是栽在这儿: TIM4 预分频
       写成了 1MHz(每格 1 微秒), 而代码把每格当 1 毫秒, 于是"等 1 秒"实际只等
       了 1 毫秒, 模块永远来不及回话 —— 日志上是"AT 打了 9000 次一个字节没有",
       看着像模块死了, 其实是量时间的尺子快了 1000 倍。 */
    {
        cal = BC28_Millis();
        Delay(1000);
        cal = (uint32_t)(BC28_Millis() - cal);
        printf("+ timebase self-check: Delay(1000) -> %lu ms  %s\r\n",
               (unsigned long)cal,
               ((cal > 900) && (cal < 1100)) ? "OK" : "** WRONG **");
        if ((cal <= 900) || (cal >= 1100))
        {
            printf("! the ms timebase is off -- every timeout below is scaled by"
                   " the same factor, fix tim4_init() before believing anything\r\n");
        }
    }

    /* ---- ① 模块在不在 ----
       别指望第一枪就中: 模块平时在 PSM/eDRX 里睡着(第 2 篇查过, 出厂就开着),
       睡着的模块被 UART 上第一个字符叫醒, 而**那个字符自己常常是丢的**。
       厂家 BC28_Init() 也是拿 while 死磕 ATE1 直到回 OK, 只是它没说为什么。
       这里连打不停, 并把"第几次才应答"打出来 —— 这个次数本身就有意义。 */
    BC28_Idle(1500);            /* 上电先给它一点时间, 别一上来就砸 */

    for (i = 1; ; i++)
    {
        info[0] = '\0';
        rc = BC28_Probe(info, sizeof(info));
        if (rc == 0)
        {
            printf("+ modem answered on AT attempt %d: %s\r\n", i, info);
            break;
        }
        if ((i == 1) || ((i % 5) == 0))
        {
            printf("- AT attempt %d: %s\r\n", i, BC28_Why(rc));
            if (i <= 2)
            {
                BC28_DumpRx();  /* 一个字节都没有 = 模块根本没在说 */
            }
        }
        if (i >= 60)            /* 60 次 x 1 秒还不应, 那就是真不通, 报一次再接着试 */
        {
            printf("! no answer after 60 AT attempts\r\n");
            printf("! check: SW8 switch / BC28 module power / PA2-PA3\r\n");
            i = 0;
        }
        BC28_Idle(500);
    }

    /* ---- ② 注网 ---- */
    printf("+ waiting for network (AT+CEREG?) ...\r\n");
    rc = BC28_WaitNet(BC28_TO_NET);
    if (rc < 0)
    {
        printf("! network attach timeout: %s\r\n", BC28_Why(rc));
        while (1) { BC28_Idle(500); }
    }
    printf("+ attached (CEREG = 1 or 5)\r\n");

    /* ---- ③ 记一次信号, 让你一眼看到天线和卡是好的 ---- */
    {
        BC28_RADIO r0;
        char       rb[64];
        char      *q = rb;

        if (BC28_GetRadio(&r0) == 0)
        {
            q += sprintf(q, "CSQ=%d  RSRP=", r0.csq);
            q += put_tenth(q, r0.rsrp);
            q += sprintf(q, " dBm  SNR=");
            q += put_tenth(q, r0.snr);
            q += sprintf(q, " dB  ECL=%d  band=%d", r0.ecl, r0.band);
            printf("+ radio: %s\r\n", rb);
        }
        else
        {
            printf("! could not read radio (going on anyway)\r\n");
        }
    }

    /* ---- ④ 主循环 ---- */
    printf("\r\n--- uplink started ---\r\n");

    for (;;)
    {
        uint32_t t0 = BC28_Millis();

        seq++;

#if MODE_LONG
        /* 运营商经常把闲着的 socket 掐掉, 掐的时候模块会给一条 +NSOCLI。
           驱动的每次收发都顺手扫它, 这里检查一下就够了 —— 不用另外去 ping。 */
        if (BC28_SocketClosed())
        {
            printf("! got +NSOCLI: socket closed by network, reconnecting\r\n");
            (void)BC28_Close(sock);
            sock = -1;
        }

        if (sock < 0)
        {
            sock = link_up();
            if (sock < 0)
            {
                printf("- connect failed, retry in 5 s\r\n");
                BC28_Idle(5000);
                continue;
            }
            GPIO_SetBits(GPIOC, GPIO_Pin_3);        /* 板上那颗灯 = 连着 */
        }

        report_once();
#else
        /* 短连接: 每轮 建连 -> 发 -> 收 -> 关, 专门用来量建连耗时 */
        {
            uint32_t dt;
            int      s2 = link_up();

            if (s2 < 0)
            {
                printf("- connect failed, retry in 3 s\r\n");
                BC28_Idle(3000);
                continue;
            }
            sock = s2;
            GPIO_SetBits(GPIOC, GPIO_Pin_3);

            dt = BC28_Millis() - t0;
            conn_n++;
            conn_sum += dt;
            if ((conn_n == 1) || (dt < conn_min)) { conn_min = dt; }
            if (dt > conn_max) { conn_max = dt; }
            printf("  [connect #%lu] %lu ms   (min %lu / max %lu / avg %lu, n=%lu)\r\n",
                   (unsigned long)conn_n, (unsigned long)dt,
                   (unsigned long)conn_min, (unsigned long)conn_max,
                   (unsigned long)(conn_sum / conn_n), (unsigned long)conn_n);

            report_once();

            (void)BC28_Close(sock);
            sock = -1;
            GPIO_ResetBits(GPIOC, GPIO_Pin_3);
        }
#endif

        /* 等下一轮。用 BC28_Idle 而不是死等 —— 它一边等一边把 URC 收了。 */
        while ((uint32_t)(BC28_Millis() - t0) < REPORT_MS)
        {
            BC28_Idle(200);
        }
    }
}

/**
  * @brief  Inserts a delay time.
  */
void Delay(__IO uint32_t nTime)
{
    TimingDelay = nTime;
    while (TimingDelay != 0);
}

/**
  * @brief  Decrements the TimingDelay variable. Called from SysTick_Handler.
  */
void TimingDelay_Decrement(void)
{
    if (TimingDelay != 0x00)
    {
        TimingDelay--;
    }
}

#ifdef  USE_FULL_ASSERT

void assert_failed(uint8_t* file, uint32_t line)
{
    while (1)
    {
    }
}
#endif
