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
  *  四个开关, 改完重编译重烧:
  *    MODE_LONG            1 = 长连接(默认) / 0 = 每轮重连, 用来量建连耗时
  *    BC28_SKIP_NSOCO_URC  在 bc28.h, =1 就是复现厂家那种"不等 URC 就发"
  *    REPORT_MS            上报间隔
  *    DHT11_SELFTEST       1 = 开机先连读 10 次温湿度(调传感器用) / 0 = 关
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
#include "dht11.h"
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
#define SERVER_HOST     "your-server.example.com"
#define SERVER_PORT     9101

/* 预共享 token。**真值只在服务器的 sink.env(600) 里, 不进任何仓库。**
   服务端每条连接的第一行必须是 "#TOKEN <它>", 不对就记一条 REJECT 然后断开。
   为什么公网裸端口一定要有这个 —— 见第 3 篇 §2。 */
#define SINK_TOKEN      "put-your-own-token-here"

/* ---- 开关 -----------------------------------------------------------------*/
/* 2026-09-14 实测: 当前这条 NB-IoT 路径上, 长连接**只过得去 1 条**就静默失效 ——
   服务端 ss 查不到这条连接、模块却继续对 AT+NSOSD 回 OK, 板子一直发、一直收不到应答;
   换成第 3 篇那版固件(不含 DHT11)复现完全一样, 所以跟传感器无关。
   短连接同一晚实测 43/43 全中。先把第 5 篇跑通, 长连接的自动重连单独做一次改动。 */
#define MODE_LONG       0               /* 1 = 长连接(推荐); 0 = 每轮建连->发->收->关 */
#define REPORT_MS       30000UL         /* 上报间隔 */

/* 开机先连读 10 次温湿度, 专门用来**调传感器**。
   调的时候把它开成 1: 读 10 次、每次隔 2 秒 = 20 秒, 正好一次
   read_com.ps1 -Seconds 25 的量 —— 一次烧录换 20 个数据点, 省掉反复烧写。
   调通了改成 0 重烧, 温湿度就并进每 30 秒那条上报里。 */
#define DHT11_SELFTEST  0

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

/* 温湿度。**必须 static** —— 本工程 Stack_Size 只有 0x400 = 1KB,
   DHT11_DATA 里光实测量就有十几个 uint16, 放栈上不合适。
   而且它是**上一轮**的读数, 天生就该是跨函数存在的状态。 */
static DHT11_DATA dht;          /* 最近一次读数(失败时里面的实测量仍然有效) */
static uint32_t   dht_ms;       /* 上次读的时刻, 用来限流 */
static int        dht_rc;       /* 最近一次读的返回值 */

static uint32_t   conn_fail;    /* 连续建连失败了几次 —— 到阈值就重启模块 */

#if !MODE_LONG
/* 短连接模式的建连耗时统计(长连接那一档用不到, 不声明就不会有 unused 警告) */
static uint32_t conn_n, conn_min, conn_max, conn_sum;
#endif

/* Private function prototypes -----------------------------------------------*/
void Delay(__IO uint32_t nTime);
void TimingDelay_Decrement(void);

static int  put_tenth(char *p, int v);
static void trim(char *s);
static void dht_temp_str(char *out, const DHT11_DATA *d);
static int  wait_reply(int s, char *out, uint16_t cap, int rounds);
static int  link_up(void);
static void report_once(void);
static int  hard_recover(void);
static void connect_failed(uint32_t wait_ms);

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
  * @brief  把 DHT11 的温度打成人看的样子: "24.0" / "-5.0"。
  *
  *         ⚠ DHT11 温度**整数部分的最高位是符号位**(负温时置 1), 不是无符号数。
  *           直接按无符号打的话, -5 度会变成 251 度 —— 数据手册第 6 页那张
  *           位定义表里写了, 但很容易漏掉。湿度那两字节没有符号位, 不用管。
  *
  *         小数部分 DHT11 恒为 0(它的分辨率就是 1 度), 留着是为了跟
  *         DHT22 的格式对齐 —— 以后换传感器上层不用改。
  */
static void dht_temp_str(char *out, const DHT11_DATA *d)
{
    int t   = (int)d->temp;
    int neg = 0;

    if ((t & 0x80) != 0)
    {
        neg = 1;
        t  &= 0x7F;
    }
    sprintf(out, "%s%d.%d", (neg != 0) ? "-" : "", t, (int)d->temp_dec);
}

/**
  * @brief  看着像 IPv4 吗?  "203.0.113.7" 是, "your-server.example.com" 不是。
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
  * @brief  最后一招: 让 BC28 整个重启, 然后等它重新注网。
  *
  *  为什么值得下这么重的手 —— 2026-09-14 实测: 模块的 socket 层会自己卡死,
  *  卡死时 AT+NSOCR 直接回 ERROR, 而**同一时刻 CSQ=25 / RSRP=-73**, 无线好得很。
  *  也就是说单看信号永远查不出来, AT 层的其它补救(NSOCL 等)也全被拒 ——
  *  只有 NRB 能把它救回来。详见 bc28.c 里 BC28_Reset() 的文件头注释。
  *
  *  返回 0 = 救回来了, < 0 = 重启了还是没注上网。
  */
static int hard_recover(void)
{
    printf("! %lu connects in a row failed -- module socket layer looks wedged\r\n",
           (unsigned long)conn_fail);
    printf("! rebooting BC28 (AT+NRB), this takes ~15 s ...\r\n");

    conn_fail = 0;
    BC28_Reset();

    if (BC28_WaitNet(60000) != 0)
    {
        printf("! rebooted but still not attached\r\n");
        return -1;
    }

    printf("+ BC28 rebooted and attached again\r\n");
    return 0;
}

/**
  * @brief  建连失败的统一出口: 记账、必要时重启模块、然后等一会儿。
  *         两个模式(MODE_LONG)都走这里, 免得同一套逻辑写两遍。
  */
static void connect_failed(uint32_t wait_ms)
{
    conn_fail++;
    printf("- connect failed (%lu in a row), retry in %lu ms\r\n",
           (unsigned long)conn_fail, (unsigned long)wait_ms);

    if (conn_fail >= 5)
    {
        (void)hard_recover();
    }

    BC28_Idle(wait_ms);
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
  *         一行的样子: #12,CSQ=28,RSRP=-68.3,SNR=6.4,ECL=0,UP=360,T=26.9
  *
  *         ⚠ **只发温度, 不发湿度。** 手上这颗传感器的湿度通道**恒为 0**,
  *         而校验和每次都是对的 —— 也就是"不是读错位, 是它真在发 0"
  *         (详见下面自检那一段的注释)。房间里不可能有 0%RH, 所以那一栏发出去
  *         就是往服务端灌假数据, 不如不发。以后换一颗好的(或 DHT22),
  *         把 `,H=` 加回来就行。
  *
  *         温度读失败时发 -99.0 并补一个 DHTE=<错误码> —— **保持字段形状
  *         不变**。服务端是按逗号切、按 `=` 取值的, 少一栏或者变个形状都会
  *         让它解析出错; 而"这天传感器坏了"本身也是值得记下来的数据,
  *         不该让整条上报发不出去。
  */
static void report_once(void)
{
    BC28_RADIO r;
    char       body[128];
    char       buf[64];
    char       ts[16];
    char      *p;
    int        rc;
    int        n;
    int        i;

    /* ---- 温湿度 ----
       限流到 DHT11_MIN_PERIOD_MS: 一来手册要求采样间隔 >= 1 秒(敏感元件
       要缓过来, 连着读只会拿到同一个数), 二来读一次要**阻塞 20ms**。
       现在上报是 30 秒一轮, 所以实际上每轮都读; 这个判断是防以后把
       上报间隔调小的 —— 那时候它会自己变成"每 2 秒才读一次"。 */
    if ((uint32_t)(BC28_Millis() - dht_ms) >= DHT11_MIN_PERIOD_MS)
    {
        dht_rc = DHT11_Read(&dht);
        dht_ms = BC28_Millis();
    }

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

    if (dht_rc == 0)
    {
        dht_temp_str(ts, &dht);
        p += sprintf(p, ",T=%s", ts);
    }
    else
    {
        p += sprintf(p, ",T=-99.0,DHTE=%d", dht_rc);
    }

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
    DHT11_Init();       /* PB8 开漏 + TIM6 微秒时基。放在 uart1_init 之后,
                           这样它要是哪天会打印, 日志也出得来 */

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

#if DHT11_SELFTEST
    /* ---- ⓪.5 温湿度自检 ----
       放在模块探测**之前**: 能不能读 DHT11 跟 NB-IoT 链路毫无关系,
       先把它调通, 后面万一出问题就不用怀疑这一侧。

       跑 10 次、每次隔 2 秒(DHT11 的采样间隔下限) = 20 秒, 正好一次
       read_com.ps1 -Seconds 25 的量 —— **一次烧录换 20 个数据点**,
       调硬件的时候这个很省事, 不然就是改一次烧一次。

       ⚠ 用**有限次**循环而不是 for(;;): 后面还有 BC28_Idle 那一句,
         写成 for(;;) 会让它变成不可达代码, AC5 报 #111-D 警告,
         而本工程要求 0 Warning。

       两行输出里的第二行(实测量)才是这一节的重点 —— 见 dht11.h 里那段:
       DHT11 的 0 和 1 只差在高电平宽度, 把 hi0<= / hi1>= 打出来就知道
       门槛 50us 两边各有多少余量。接错线的时候也是靠这几个数一眼看出来
       "时序根本不对", 而不是傻猜"传感器是不是坏了"。

       ---- 手上这颗传感器实测出来的两件事(第 5 篇要写进去) ----

       1) **它的温度是 0.1 分辨率, 所以它不是纯 DHT11。** 真 DHT11 的温度
          分辨率是 1 摄氏度, 小数那两个字节恒为 0。这颗给的 raw 是
          `1A 09` -> `1B 00` -> `1B 01` ... 也就是 26.9 -> 27.0 -> 27.1,
          中间那一步是**进位**(小数从 9 滚到 0, 整数 +1)。协议一样但分辨率
          不一样, 说明它是 DHT12 或同协议的克隆片。**驱动不用改** ——
          dht11.c 本来就把 4 个字节都收全了, 小数位照收(from day one)。

       2) **它的湿度通道恒为 0, 但校验和是对的。** raw 的前两个字节每次都是
          `00 00`, 而第 5 个字节仍然等于前 4 个之和 —— 40 位里错一位还能撞对
          校验和的概率是 1/256, 连中 10 次不可能。所以结论只能是一个:
          **不是读错了, 是它真在发 0**。房间里不可能有 0%RH(传感器量程下限
          本身就是 20%), 所以这一栏是坏的, 上报里干脆不发 `,H=`。
          —— 这一条本身就是个挺好的教训: **校验和通过 != 数据有意义**,
             dht11.c 里那道 E_RANGE 量程检查就是为这类"看着合法其实全错"
             准备的(只是它拦的是超量程, 拦不住"合法的 0")。 */
    {
        char ts[16];
        int  k;

        printf("\r\n--- DHT11 self-test: 10 reads, 2 s apart ---\r\n");
        for (k = 1; k <= 10; k++)
        {
            dht_rc = DHT11_Read(&dht);
            dht_ms = BC28_Millis();

            if (dht_rc == 0)
            {
                dht_temp_str(ts, &dht);
                printf("+ [%d/10] T=%sC   raw=%02X %02X %02X %02X %02X\r\n",
                       k, ts,
                       (unsigned)dht.raw[0], (unsigned)dht.raw[1],
                       (unsigned)dht.raw[2], (unsigned)dht.raw[3],
                       (unsigned)dht.raw[4]);
            }
            else
            {
                printf("- [%d/10] FAIL (%d): %s\r\n", k, dht_rc, DHT11_Why(dht_rc));
                if (dht_rc == DHT11_E_NORESP)
                {
                    printf("          check: sensor powered? data wire on B8");
                    printf(" (the middle pin)? module pin order right?\r\n");
                }
                else if (dht_rc == DHT11_E_STUCK)
                {
                    printf("          check: line shorted to GND? module's own");
                    printf(" pull-up powered? (this board has none)\r\n");
                }
            }

            /* 这一行**成功失败都打** —— 失败时它才是唯一线索。 */
            printf("          start=%u resp=%u/%u hi0<=%u hi1>=%u lo<=%u frame=%u\r\n",
                   (unsigned)dht.start_us, (unsigned)dht.resp_low,
                   (unsigned)dht.resp_high, (unsigned)dht.hi0_max,
                   (unsigned)dht.hi1_min, (unsigned)dht.lo_max,
                   (unsigned)dht.frame_us);

            if (k < 10)
            {
                BC28_Idle(DHT11_MIN_PERIOD_MS);     /* 一边等一边收 URC */
            }
        }
        printf("--- DHT11 self-test done ---\r\n\r\n");
    }
#endif

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
    /* 2026-09-15 改: 这里以前是 `while(1) { BC28_Idle(500); }` —— 开机注网一旦超时,
       就永久静默死等, 串口不出声、看板没数据、AT 也不回, **看起来跟板子坏了一模一样**,
       而且断电重开还是这样(开机 BC28_Init 会 AT+NRB 重启模块, 每次都得重新注网)。
       实测代价: 用户以为板子坏了, 查了一晚上。
       现在改成一直重试: 超时就把模块 AT+NRB 重启再来一轮。
       板子宁可慢一点起来, 也绝不出这种"安静地死掉"的状态。
       注: 主循环里早就有同一套重试(connect_failed -> hard_recover), 只有启动这条路漏了。 */
    {
        uint32_t round = 0;

        for (;;)
        {
            round++;
            printf("+ waiting for network (AT+CEREG?) ... round %lu\r\n",
                   (unsigned long)round);

            rc = BC28_WaitNet(BC28_TO_NET);
            if (rc == 0)
            {
                break;
            }

            printf("! network attach timeout: %s\r\n", BC28_Why(rc));
            printf("! rebooting BC28 (AT+NRB) and trying again ...\r\n");
            BC28_Reset();
            BC28_Idle(1000);
        }
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
                connect_failed(5000);
                continue;
            }
            conn_fail = 0;
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
                connect_failed(3000);
                continue;
            }
            conn_fail = 0;
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
