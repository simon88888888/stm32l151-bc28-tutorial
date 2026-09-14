/**
  ******************************************************************************
  * @file    HARDWARE/bc28/bc28.c
  * @brief   BC28 (NB-IoT) 驱动 —— 自己写的, 不搬厂家 HARDWARE/BC28。
  *          配套手册: 第 3 篇 · 把数据发到自己的服务器
  *
  *  这个文件只做四件事:
  *
  *    1) 一个**不丢 tick 的毫秒时基** —— 用 TIM4 自由跑, 不用 SysTick 的
  *       COUNTFLAG 轮询。第 4 篇那个 TickPoll 是为了不碰 stm32l1xx_it.c,
  *       代价是"没及时读就把那次回绕丢了"; 而本篇要**量**建连耗时(2~22 秒),
  *       丢 tick 会让测出来的时间系统性偏短, 正好毁掉要测的那个数。
  *       TIM4 的 CNT 一直数着, 我们只在读的时候做一次 16 位无符号差,
  *       每秒读几次就够精确。
  *
  *    2) 自己的一份收缓冲。**不用 usart.c 的 buf_uart2** —— 它是 256 字节
  *       线性缓冲, 满了 `index = 0` 静默回绕。等 +NSOCO URC 最长 22 秒,
  *       9600 波特下能来 ~2KB, 回绕 8 次, strstr 会匹配到老数据的碎片,
  *       结果是"假超时"或者更糟的"假成功"。所以把 RXNE 中断关掉, 直接轮询
  *       USART2->DR。TX 那半照旧用 usart.c 的 Uart2_SendStr()。
  *
  *    3) 一个"等固定串、或 ERROR、或超时"的等待 —— 所有 AT 都走它。
  *       厂家每种状态循环 100 次 x 300ms, 循环里既不重发也不清缓冲。
  *
  *    4) 建连时**等 "+NSOCO: <n>" 这条 URC**。AT+NSOCO 回的那个 OK 只是
  *       "受理了", 在 URC 之前 AT+NSOSD 发出去的数据会全丢, 而 NSOSD 照样
  *       回 OK。留了 BC28_SKIP_NSOCO_URC 把厂家那种写法复现出来做对照。
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "bc28.h"
#include "usart.h"          /* uart2_init() / Uart2_SendStr() —— 只借这两样 */
#include "main.h"           /* 工程惯例: main.h 里放着公共声明 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Private define ------------------------------------------------------------*/
/* 收缓冲。**必须 static** —— 本工程 Stack_Size 只有 0x400 = 1KB, 放栈上会爆。
   +1 是为了永远留一个 '\0', strstr 才不会跑出界。 */
#define BC28_RXCAP      1024
#define BC28_LINECAP    640     /* 一条 AT 的完整回显, NUESTATS 最长(~380B) */

/* Private variables ---------------------------------------------------------*/
static char     rx[BC28_RXCAP + 1];   /* 收到的原始字节 */
static uint16_t rx_len;               /* 写指针: 已经收了多少 */
static uint16_t rx_tail;              /* 读指针: 已经被消费到哪儿 */
static uint8_t  rx_ovf;               /* 溢出过(有数据被丢) */
static uint8_t  sock_closed;          /* 收到过 +NSOCLI */

/* "+NSOCO: <n>" 这条 URC 从发出命令到到达, 到底花了多久 —— 第 2 篇是在 PC 上
   用 -w 大概估的(2~22 秒), 这里由固件自己精确量出来, 给第 3 篇的实测当数据。
   nsoco_seen 初值给 1: 上电时还没有任何一次连接, 别让"没等到"被当成"0 毫秒"。 */
static uint8_t  nsoco_seen = 1;
static uint32_t nsoco_t0;
static uint32_t nsoco_ms;

static char     line[BC28_LINECAP];   /* 最近一条 AT 的完整回显, 给调用方解析 */

/* 毫秒时基: TIM4 自由跑, 只读 CNT, 靠 16 位无符号差累计回绕。
   调用间隔只要 < 65.5 秒(一个回绕周期)就不会丢 —— 所有等待循环都在读它。 */
static uint32_t ms_total;
static uint16_t ms_last;

/* Private function prototypes -----------------------------------------------*/
static void rx_pump(void);
static void urc_scan(void);
static void rx_drop_all(void);
static void rx_consume(uint16_t n);
static int  wait_for(const char *expect, uint32_t to_ms, char *out, uint16_t cap);
static int  at_cmd(const char *cmd, const char *expect, uint32_t to_ms,
                   char *out, uint16_t cap);
static const char *seek_num(const char *p);
static const char *skip_num(const char *p);
static int  take_num(const char *p, int *out);
static int  num_after(const char *s, const char *prefix, int *out);
static int  nstats_val(const char *s, const char *key, int *out);
static int  last_digit_line(const char *s);
static const char *field(const char *s, int n);
static int  hexval(char c);

/* =========================================================================
 * 一、时基: TIM4 自由跑, 1 tick = 1ms
 * ========================================================================= */

/**
  * @brief  把 TIM4 配成 **1kHz**(每格 1 毫秒)自由计数器, 不开中断、不碰
  *         stm32l1xx_it.c。STM32L151 上 TIM4 在本工程里没人用
  *         (串口是 USART1/2/3, 墨水屏是软件 SPI, SysTick 被 Delay 占了)。
  *
  *  ⚠ 预分频这里除的是 1000, 不是 1000000 —— 一个字的差别, 差 1000 倍。
  *     第一版我写成了 /1000000(计数 1MHz, 每格 1 微秒), 而 BC28_Millis() 把
  *     每一格当成 1 毫秒来累加。后果: 时基快 1000 倍, 于是所有超时都缩短
  *     1000 倍 —— 写着"等 1 秒"其实只等了 1 毫秒, 模块永远来不及回话。
  *     现象是"AT 打了 9000 次, 一个字节都没回来", 看着像模块死了/线接错了,
  *     实际是量时间的尺子错了。main.c 里那个时基自检就是为这件事留的。
  */
static void tim4_init(void)
{
    TIM_TimeBaseInitTypeDef tb;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM4, ENABLE);

    TIM_TimeBaseStructInit(&tb);
    tb.TIM_Prescaler     = (uint16_t)(SystemCoreClock / 1000) - 1;  /* 1kHz: 每格 1ms */
    tb.TIM_Period        = 0xFFFF;      /* 65536 格回绕一次 = 65.5 秒 */
    tb.TIM_CounterMode   = TIM_CounterMode_Up;
    tb.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInit(TIM4, &tb);
    TIM_Cmd(TIM4, ENABLE);

    ms_last  = (uint16_t)TIM4->CNT;
    ms_total = 0;
}

/**
  * @brief  毫秒时基, 从上电(BC28_Init)算起。
  *         做法: 读一次 16 位 CNT, 跟上次的差(无符号, 天然处理回绕)累加到 32 位。
  *         精度取 1ms 的整数毫秒部分, 问的是"过了多久", 不是"现在几点"。
  */
uint32_t BC28_Millis(void)
{
    uint16_t now = (uint16_t)TIM4->CNT;
    uint16_t d   = (uint16_t)(now - ms_last);

    if (d != 0)
    {
        ms_total += d;
        ms_last   = now;
    }
    return ms_total;
}

/* =========================================================================
 * 二、收: 自己的线性缓冲
 * ========================================================================= */

/**
  * @brief  把 USART2 收到的字节搬进 rx[]。**不阻塞**, 到处都可以调。
  */
static void rx_pump(void)
{
    while (USART_GetFlagStatus(USART2, USART_FLAG_RXNE) != RESET)
    {
        if (rx_len < BC28_RXCAP)
        {
            rx[rx_len++] = (char)(USART2->DR & 0xFF);
            rx[rx_len]   = '\0';        /* 永远给 strstr 留个终点 */
        }
        else
        {
            (void)USART2->DR;           /* 满了就丢, 但**记下来**, 不偷偷回绕 */
            rx_ovf = 1;
        }
    }

    /* 溢出错误: 读一次 SR(上面那句) 再读 DR 就清了。丢了就记一笔。 */
    if (USART_GetFlagStatus(USART2, USART_FLAG_ORE) != RESET)
    {
        (void)USART2->DR;
        rx_ovf = 1;
    }
}

/**
  * @brief  扫一眼缓冲区里有没有"自己会来"的那两条 URC, 有就记下来。
  *
  *   +NSOCLI      socket 被关了(运营商掐的, 或者对端断的)。不记下来的话,
  *                后面每一轮 NSOSD 都回 OK 但数据进的是个死 socket。
  *   +NSOCO:      **建连真正完成的凭据**。记下它离发命令过了多久 ——
  *                这就是第 2 篇那个"2~22 秒"在固件里的精确版本。
  *
  *   必须在消费缓冲**之前**调, 因为 wait_for 命中后就把那段吃掉了。
  */
static void urc_scan(void)
{
    if (strstr(rx + rx_tail, "+NSOCLI") != NULL)
    {
        sock_closed = 1;
    }
    if ((nsoco_seen == 0) && (strstr(rx + rx_tail, "+NSOCO:") != NULL))
    {
        nsoco_seen = 1;
        nsoco_ms   = (uint32_t)(BC28_Millis() - nsoco_t0);
    }
}

/**
  * @brief  把缓冲区里出现过的 URC 记成标志, 然后整个清掉。
  *         清缓冲是**每条 AT 之前**做, 这样上一轮的残留不会骗到这一轮 ——
  *         但也正因为要清, 清之前必须先扫一眼有没有 +NSOCLI(不然就丢了)。
  */
static void rx_drop_all(void)
{
    urc_scan();
    rx_len  = 0;
    rx_tail = 0;
    rx[0]   = '\0';
}

/**
  * @brief  消费掉开头 n 个字节; 消费干净了就把缓冲复位到 0 号位。
  */
static void rx_consume(uint16_t n)
{
    rx_tail = (uint16_t)(rx_tail + n);

    if (rx_tail >= rx_len)
    {
        rx_len  = 0;
        rx_tail = 0;
        rx[0]   = '\0';
    }
    else
    {
        /* 把没消费的挪到头上, 省得缓冲被"读过的部分"占满 */
        memmove(rx, rx + rx_tail, (size_t)(rx_len - rx_tail));
        rx_len = (uint16_t)(rx_len - rx_tail);
        rx_tail = 0;
        rx[rx_len] = '\0';
    }
}

/* =========================================================================
 * 三、等: 等一个固定串 / ERROR / 超时
 * ========================================================================= */

/**
  * @brief  在收到的字节里找 expect。命中就把 [rx_tail, 命中末尾) 原样拷给 out。
  *
  *   返回  0            = 找到 expect
  *          BC28_E_ERROR = 先等到了 "ERROR"
  *          BC28_E_TIMEOUT = 到点还没等到(残留留在缓冲里, 可以用 BC28_DumpRx 看)
  *
  *   三个约定, 都是对着厂家那版的毛病去的:
  *     - 有**上限**, 到点就返回, 不无限循环;
  *     - 一路上都盯着 "+NSOCLI", 运营商把 socket 掐了当场就知道;
  *     - 超时时**不假装成功**(厂家 BC28_ConTCP 里 errcount>100 直接 break,
  *       连 err 都没置)。
  */
static int wait_for(const char *expect, uint32_t to_ms, char *out, uint16_t cap)
{
    uint32_t t0 = BC28_Millis();
    char    *p;
    uint16_t n;

    for (;;)
    {
        rx_pump();
        urc_scan();

        p = strstr(rx + rx_tail, expect);
        if (p != NULL)
        {
            n = (uint16_t)(p - (rx + rx_tail)) + (uint16_t)strlen(expect);
            if ((out != NULL) && (cap > 0))
            {
                uint16_t m = (n >= cap) ? (uint16_t)(cap - 1) : n;
                memcpy(out, rx + rx_tail, m);
                out[m] = '\0';
            }
            rx_consume(n);
            return 0;
        }

        p = strstr(rx + rx_tail, "ERROR");
        if (p != NULL)
        {
            n = (uint16_t)(p - (rx + rx_tail)) + 5;
            if ((out != NULL) && (cap > 0))
            {
                uint16_t m = (n >= cap) ? (uint16_t)(cap - 1) : n;
                memcpy(out, rx + rx_tail, m);
                out[m] = '\0';
            }
            rx_consume(n);
            return BC28_E_ERROR;
        }

        if ((uint32_t)(BC28_Millis() - t0) >= to_ms)
        {
            return BC28_E_TIMEOUT;
        }
    }
}

/**
  * @brief  发一条 AT 并等结果。**发之前先清缓冲** —— 厂家 BC28.c 从来不
  *         Clear_Buffer(), 所以它 strstr 的是整个上电以来的历史。
  *
  *  先发一个空行再清: 让模块把上一轮没说完的话说完, 清掉的才是真的"旧的"。
  */
static int at_cmd(const char *cmd, const char *expect, uint32_t to_ms,
                  char *out, uint16_t cap)
{
    Uart2_SendStr("\r\n");
    BC28_Idle(20);              /* 20ms 足够 9600 波特把残留收完 */
    rx_drop_all();

    Uart2_SendStr((char *)cmd);
    Uart2_SendStr("\r\n");

    return wait_for(expect, to_ms, out, cap);
}

/**
  * @brief  读"当前位置到下一个换行"之间的一小段, 最多等 to_ms。
  *
  *   存在的理由: 有些 AT 的**结果是跟在 OK 后面另发一条的**, 不是同一条回显。
  *   +QDNS 就是 —— 手册的 Example 原样写成两段:
  *
  *       AT+QDNS=0,www.baidu.com
  *       OK                        <- 只是"受理了"
  *       +QDNS:111.13.100.91       <- 结果另发一条
  *
  *   而 wait_for 是**一命中期望串就返回、并且只消费到命中末尾**, 所以
  *   wait_for("+QDNS:") 命中时缓冲里只剩 IP 那半截 —— 这一小段得自己接着读。
  *
  *   (顺带一提: 这跟第 2 篇 +NSOCO 那个坑是**同一个形状** ——
  *    "OK 只表示受理, 真正的结果另发一条"。同一种坑我在这篇里踩了两次。)
  *
  *   返回  读到的字符数(> 0); BC28_E_TIMEOUT = 到点还没有。
  */
static int read_field(char *out, uint16_t cap, uint32_t to_ms)
{
    uint32_t t0 = BC28_Millis();
    uint16_t n  = 0;

    out[0] = '\0';

    for (;;)
    {
        rx_pump();

        while (rx_tail < rx_len)
        {
            char c = rx[rx_tail];

            if ((c == '\r') || (c == '\n'))
            {
                rx_consume(1);              /* 行尾/空行: 吃掉 */
                if (n > 0)
                {
                    out[n] = '\0';          /* 有内容了, 这个换行就是结尾 */
                    return (int)n;
                }
            }
            else if ((n == 0) && (c == ' '))
            {
                rx_consume(1);              /* 行首空格, 跳过 */
            }
            else if (n < (uint16_t)(cap - 1))
            {
                out[n] = c;
                n++;
                rx_consume(1);
            }
            else
            {
                rx_consume(1);              /* 装不下了: 丢掉, 但继续读到换行为止 */
            }
        }

        if ((uint32_t)(BC28_Millis() - t0) >= to_ms)
        {
            out[n] = '\0';
            return (n > 0) ? (int)n : BC28_E_TIMEOUT;
        }
        urc_scan();
    }
}

/* =========================================================================
 * 四、解析: 老老实实扫数字, 不数第几个字符
 *   厂家 BC28.c 里是 socketnum = strx[14] —— 硬数第 14 个字符。
 *   回显多一个空格就错, 解析失败时还留着上一次的全局值继续用。
 * ========================================================================= */

static const char *seek_num(const char *p)
{
    while (*p != '\0')
    {
        if (((*p >= '0') && (*p <= '9')) || (*p == '-'))
        {
            return p;
        }
        p++;
    }
    return NULL;
}

static const char *skip_num(const char *p)
{
    while (((*p >= '0') && (*p <= '9')) || (*p == '-'))
    {
        p++;
    }
    return p;
}

static int take_num(const char *p, int *out)
{
    char *end;
    long  v;

    v = strtol(p, &end, 10);
    if (end == p)               /* 不是数字(比如只有个 '-') */
    {
        return BC28_E_PARSE;
    }
    *out = (int)v;
    return 0;
}

/** @brief  找 prefix, 取它后面第一个十进制数。例: ("+CSQ: 28,0", "+CSQ: ") -> 28 */
static int num_after(const char *s, const char *prefix, int *out)
{
    const char *p = strstr(s, prefix);

    if (p == NULL)
    {
        return BC28_E_PARSE;
    }
    p = seek_num(p + strlen(prefix));
    if (p == NULL)
    {
        return BC28_E_PARSE;
    }
    return take_num(p, out);
}

/** @brief  "+NUESTATS: Signal power:-683" 这种, 按冒号后面的键名取值。
  *         注意单位是 0.1(第 2 篇 §4.4), 这里原样返回。 */
static int nstats_val(const char *s, const char *key, int *out)
{
    const char *p = strstr(s, key);

    if (p == NULL)
    {
        return BC28_E_PARSE;
    }
    p = seek_num(p + strlen(key));
    if (p == NULL)
    {
        return BC28_E_PARSE;
    }
    return take_num(p, out);
}

/**
  * @brief  找"单独一行、整行都是数字"的那一行, 返回它的值, 没有就返回 -1。
  *         AT+NSOCR 分配到的 socket 号就是这么回来的(<sock>\r\nOK)。
  *         用"整行都是数字"当判据, 而不是"第 14 个字符"。
  */
static int last_digit_line(const char *s)
{
    const char *p = s;
    int val = -1;

    while (*p != '\0')
    {
        const char *e = p;
        const char *q;
        int         all = 1;

        while ((*e != '\0') && (*e != '\r') && (*e != '\n'))
        {
            e++;
        }
        if (e > p)
        {
            for (q = p; q < e; q++)
            {
                if ((*q < '0') || (*q > '9')) { all = 0; break; }
            }
            if (all)
            {
                int v = 0;
                for (q = p; q < e; q++)
                {
                    v = v * 10 + (int)(*q - '0');
                }
                val = v;
            }
        }
        p = (*e != '\0') ? (e + 1) : e;
    }
    return val;
}

/**
  * @brief  按逗号切第 n 个字段(n 从 0 开始), 返回字段起点。
  *         AT+NSORF 那行是六个字段, 位置固定:
  *           <sock>,<ip>,<port>,<len>,<hex data>,<remaining>
  */
static const char *field(const char *s, int n)
{
    const char *p = s;

    while (n-- > 0)
    {
        p = strchr(p, ',');
        if (p == NULL)
        {
            return NULL;
        }
        p++;
    }
    return p;
}

static int hexval(char c)
{
    if ((c >= '0') && (c <= '9')) { return (int)(c - '0'); }
    if ((c >= 'A') && (c <= 'F')) { return (int)(c - 'A') + 10; }
    if ((c >= 'a') && (c <= 'f')) { return (int)(c - 'a') + 10; }
    return -1;
}

/* =========================================================================
 * 五、对外的接口
 * ========================================================================= */

/**
  * @brief  让模块整个重启一次, 拿一个干净状态。
  *
  *  AT+NRB **不回普通响应**: 它先回一个 OK, 然后模块自己断电重来, 起来时吐
  *  一条 "RDY"。所以要等两次、而且等得久(实测 ~10 秒)。
  *
  *  ★ 为什么非要有它 —— 这是 2026-09-14 晚上实测出来的一个大坑:
  *    模块的 socket 层会**自己卡死**。卡死之后:
  *        AT+NSOCR=STREAM,6,0,1   ->  ERROR   (6 个 socket 全占着, 一个都分不出来)
  *        AT+NSOCL=<n>            ->  关不掉  (所以服务端那头永远收不到 FIN,
  *                                             连接会一直挂到 600 秒超时才被收走)
  *    而**同一时刻 CSQ=25 / RSRP=-73** —— 无线完全正常, 板子串口还在每 3 秒
  *    重试一次。也就是说 AT 层任何补救都没用, 只能让它重启。
  *
  *    更要命的是: **MCU 复位不会复位模块**(是两套独立的复位), 所以这个卡死
  *    状态会跨烧录、跨复位键一直留着 —— 表现出来就是"换了固件也一样坏",
  *    非常容易误判成代码问题(我们就是这么误判过一轮的)。
  */
void BC28_Reset(void)
{
    char tmp[32];

    rx_drop_all();

    /* 模块要是本来就在重启, 这句收不到 OK, 无所谓 —— 下面照样等 RDY */
    (void)at_cmd("AT+NRB", "OK", BC28_TO_LONG, NULL, 0);

    (void)wait_for("RDY", 30000, tmp, sizeof(tmp));
    BC28_Idle(1000);            /* 起来之后还得喘一口, 立刻发 AT 会丢 */
    rx_drop_all();
}

/**
  * @brief  初始化: 串口 + 时基 + **关掉 USART2 的 RXNE 中断**。
  */
void BC28_Init(void)
{
    uart2_init(9600);

    /* 这一句是这个驱动的地基, 理由见文件头第 2 条:
       usart.c:264 的 USART2_IRQHandler 会往 buf_uart2 里塞, 而那个缓冲
       满了就把 index 归零。我们要自己读 DR, 所以先把中断这条路掐掉。
       注意只掐 RXNE, TX 那条路不用中断, Uart2_SendStr() 照旧能用。 */
    USART_ITConfig(USART2, USART_IT_RXNE, DISABLE);
    NVIC_DisableIRQ(USART2_IRQn);

    tim4_init();

    rx_len      = 0;
    rx_tail     = 0;
    rx[0]       = '\0';
    rx_ovf      = 0;
    sock_closed = 0;

    /* 开机先把模块重启一次。
       一开始这里写的是"无脑 AT+NSOCL=0..5 关一遍", 实测**不够** ——
       模块卡死的时候 NSOCL 本身就被拒, 关不动。只有 NRB 能救。
       代价是每次开机多等十几秒重新注网, 换来的是"模块一定从干净状态开始"。 */
    BC28_Reset();

    (void)BC28_Millis();        /* 把 ms_last 对齐到当前 CNT */
}

/**
  * @brief  空转 ms 毫秒, 一边收字节一边走时钟。主循环里等下一轮上报就用它,
  *         这样 +NSOCLI 这种"什么时候都可能来"的 URC 不会被漏掉。
  */
void BC28_Idle(uint32_t ms)
{
    uint32_t t0    = BC28_Millis();
    uint32_t tscan = t0;

    for (;;)
    {
        uint32_t now = BC28_Millis();

        if ((uint32_t)(now - t0) >= ms)
        {
            break;
        }
        rx_pump();

        /* URC 不是"回给命令"的, 它什么时候来都行, 所以空转的时候也要扫。
           20ms 一次, 够密; 每圈都 strstr 一遍 1KB 是白费。 */
        if ((uint32_t)(now - tscan) >= 20)
        {
            tscan = now;
            urc_scan();
        }
    }
}

uint8_t BC28_SocketClosed(void)
{
    return sock_closed;
}

uint8_t BC28_RxOverflow(void)
{
    return rx_ovf;
}

/**
  * @brief  上一次 AT+NSOCO 之后, "+NSOCO:" 这条 URC 隔了多少毫秒才来。
  *         0 = 还没来(或者刚发完命令)。这就是第 2 篇 "2~22 秒" 的固件版。
  */
uint32_t BC28_NsocoDelayMs(void)
{
    return (nsoco_seen != 0) ? nsoco_ms : 0;
}

const char *BC28_Why(int rc)
{
    switch (rc)
    {
    case BC28_E_ARG:     return "bad-arg";
    case BC28_E_TIMEOUT: return "timeout";
    case BC28_E_ERROR:   return "modem-ERROR";
    case BC28_E_NOSOCK:  return "no-socket";
    case BC28_E_PARSE:   return "parse-failed";
    default:             return "?";
    }
}

/**
  * @brief  把一段字节打成人看得懂的样子: 不可打印字符换成 '.', \r\n 换成一行的 "\n"。
  *         给 DumpRx / DumpLine 共用。
  */
static void dump_bytes(const char *s, uint16_t n)
{
    uint16_t i;

    for (i = 0; i < n; i++)
    {
        char c = s[i];
        if ((c == '\r') || (c == '\n'))
        {
            printf("\\n");
        }
        else if ((c >= 0x20) && (c < 0x7F))
        {
            printf("%c", c);
        }
        else
        {
            printf(".");
        }
        if (i > 200)                /* 最多打 200 字节 */
        {
            printf("...");
            break;
        }
    }
}

/**
  * @brief  把缓冲里的残留打出来(诊断用)。超时的时候调一下,
  *         就知道模块到底回了什么。
  */
void BC28_DumpRx(void)
{
    printf("  raw<");
    dump_bytes(rx + rx_tail, (uint16_t)(rx_len - rx_tail));
    printf(">\r\n");
}

/**
  * @brief  AT -> ATE0 -> ATI。ATE0 关回显很关键: 不关的话, 我们发出去的命令
  *         会原样回来, strstr 找 "OK" 就可能匹到自己命令里的字符。
  *         info 收 ATI 的回显(模块型号/固件版本), 可以给 NULL。
  */
int BC28_Probe(char *info, uint16_t cap)
{
    int rc;

    rc = at_cmd("AT", "OK", BC28_TO_AT, NULL, 0);
    if (rc < 0)
    {
        return rc;
    }

    rc = at_cmd("ATE0", "OK", BC28_TO_AT, NULL, 0);
    if (rc < 0)
    {
        return rc;
    }

    return at_cmd("ATI", "OK", BC28_TO_LONG, info, cap);
}

/**
  * @brief  轮询 AT+CEREG? 直到注网。0 = 已注网。
  *         1 = 已注册(本地), 5 = 已注册(漫游, NB-IoT 常见)。
  *         用轮询而不是等 URC, 是因为 URC 要提前 AT+CEREG=1 打开, 而这块板
  *         每次上电都要重新配 —— 轮询更笨但更稳。
  */
int BC28_WaitNet(uint32_t to_ms)
{
    uint32_t t0 = BC28_Millis();
    int      rc;

    for (;;)
    {
        rc = at_cmd("AT+CEREG?", "OK", BC28_TO_LONG, line, sizeof(line));
        if (rc == 0)
        {
            const char *p = strstr(line, "+CEREG:");
            const char *q;
            int         stat = -1;

            if (p != NULL)
            {
                q = seek_num(p + 7);                    /* 第 1 个数: <n> */
                if (q != NULL)
                {
                    q = seek_num(skip_num(q));          /* 第 2 个数: <stat> */
                    if (q != NULL)
                    {
                        (void)take_num(q, &stat);
                    }
                }
            }
            if ((stat == 1) || (stat == 5))
            {
                return 0;
            }
        }

        if ((uint32_t)(BC28_Millis() - t0) >= to_ms)
        {
            return BC28_E_TIMEOUT;
        }
        BC28_Idle(1000);            /* 别把串口刷满, 模块也要喘气 */
    }
}

/** @brief  AT+CSQ -> csq(0~31, 99 = 未知) */
int BC28_GetCsq(void)
{
    int rc;
    int v;

    rc = at_cmd("AT+CSQ", "OK", BC28_TO_LONG, line, sizeof(line));
    if (rc < 0)
    {
        return rc;
    }
    if (num_after(line, "+CSQ:", &v) < 0)
    {
        return BC28_E_PARSE;
    }
    return v;
}

/**
  * @brief  AT+NUESTATS + AT+CSQ, 填进 r。字段是 0.1 单位, 原样放进来。
  *         NUESTATS 是本模块回得最慢的一条(13 行), 所以超时给 5 秒。
  */
int BC28_GetRadio(BC28_RADIO *r)
{
    int rc;

    if (r == NULL)
    {
        return BC28_E_ARG;
    }
    memset(r, 0, sizeof(BC28_RADIO));

    rc = at_cmd("AT+NUESTATS", "OK", BC28_TO_STATS, line, sizeof(line));
    if (rc < 0)
    {
        return rc;
    }

    (void)nstats_val(line, "Signal power", &r->rsrp);
    (void)nstats_val(line, "SNR",          &r->snr);
    (void)nstats_val(line, "ECL",          &r->ecl);
    (void)nstats_val(line, "CURRENT BAND", &r->band);

    r->csq = BC28_GetCsq();     /* 这条放最后: 它会清缓冲, 得先取完 NUESTATS */
    return 0;
}

/**
  * @brief  开一个 TCP socket, 返回 socket 号(>= 0)。
  *         AT+NSOCR=STREAM,6,0,1 的四个参数:
  *           类型 TCP / 协议 TCP / 本地端口 0=自动 / **接收模式 1**
  *         接收模式 1 = 数据来了先给一条 URC, 要用 AT+NSORF 去取。
  *         注意返回的号**不保证是 0**(第 2 篇实测到过 3), 一定要接着用。
  */
int BC28_Open(void)
{
    int rc;

    sock_closed = 0;

    rc = at_cmd("AT+NSOCR=STREAM,6,0,1", "OK", BC28_TO_LONG, line, sizeof(line));
    if (rc < 0)
    {
        return rc;
    }

    rc = last_digit_line(line);
    if (rc < 0)
    {
        return BC28_E_NOSOCK;
    }
    return rc;
}

/**
  * @brief  把域名解析成 IP。AT+QDNS=0,<hostname> -> OK 然后 "+QDNS:<ip>"
  *
  *  为什么要这个 —— 家宽的出口 IP 是动态的, 光猫重启一次就变。把 IP 写死在固件里,
  *  意味着每次变了都要重编译重烧, 那就等于没法长期挂着。
  *
  *  好消息是**这件事不用自己做**: 模块自带 DNS 解析(这条 AT 就是它), 所以
  *     - 不需要在固件里塞一个 DNS 客户端(几百行, 还要处理 UDP 重传)
  *     - 不需要给模块开 IPv6(移动 NB-IoT 本来也没给模块分 v6, 见第 3 篇)
  *     - 不需要额外挂一个 DDNS 客户端; 只要域名那边有个带 A 记录的 DDNS 即可
  *
  *  ⚠ **这一条要等两次、读两段**, 两个坑都在手册那一页的 Example 里明写着,
  *    我第一版还是两个都踩了:
  *
  *    1) 结果是**两条**。手册的 Example 原样是这样的:
  *           AT+QDNS=0,www.baidu.com
  *           OK                        <- 只是"受理了"(最大响应时间 300ms)
  *           +QDNS:111.13.100.91       <- 结果**另发一条**
  *       ——跟 AT+NSOCO 一模一样的套路。我按"等到 OK 时整行已经在缓冲里"写,
  *       跑出来只拿到光秃秃一个 "\n\nOK", 解析必然失败。
  *
  *    2) 就算等到了 "+QDNS:", wait_for 也**只消费到冒号为止**(一命中就收手),
  *       IP 那半截还留在缓冲里, 得用 read_field() 自己接着读。
  */
int BC28_Resolve(const char *host, char *out, uint16_t cap)
{
    char cmd[96];
    int  n;
    int  rc;

    if ((host == NULL) || (out == NULL) || (cap < 8))
    {
        return BC28_E_ARG;
    }
    out[0] = '\0';

    /* 域名最长 63 字符, 加 "AT+QDNS=0," 11 个字符, 96 够 */
    sprintf(cmd, "AT+QDNS=0,%s", host);

    /* ① OK = 受理了。这一条 300ms 就回, 快得很 —— 它**不含**解析耗时。 */
    rc = at_cmd(cmd, "OK", BC28_TO_LONG, NULL, 0);
    if (rc < 0)
    {
        return rc;
    }

    /* ② "+QDNS:" = 结果这一条到了。**DNS 解析本身要花时间, 多则 2~3 秒**,
       所以这里的超时必须给足 —— 用 URC 那档 30 秒, 不能用 LONG。
       (第一版把解析结果也当成"跟 OK 同一条", 等到的只有一个光秃秃的 OK。) */
    rc = wait_for("+QDNS:", BC28_TO_URC, NULL, 0);
    if (rc < 0)
    {
        BC28_DumpRx();
        return rc;
    }

    /* ③ 冒号后面那半截就是 IP —— 接着读到换行为止 */
    n = read_field(out, cap, BC28_TO_LONG);
    if (n < 0)
    {
        BC28_DumpRx();
        return n;
    }

    /* 有些固件解析不出来时给的是 0.0.0.0。这不是"解析成功",
       当成失败报出去 —— 否则会拿 0.0.0.0 去 NSOCO, 白等 30 秒才超时。 */
    if (strcmp(out, "0.0.0.0") == 0)
    {
        return BC28_E_PARSE;
    }

    return 0;
}

/**
  * @brief  连服务器。
  *
  *   正常(BC28_SKIP_NSOCO_URC = 0): 发完 AT+NSOCO 之后**必须等到
  *   "+NSOCO: <sock>" 这条 URC** 才算连上。AT 回的那个 OK 只是"受理了" ——
  *   在 URC 之前 AT+NSOSD 送出去的数据会全丢, 而 NSOSD 照样回 OK。
  *   第 2 篇实测这段要 2~22 秒, 所以超时给 30 秒。
  *
  *   对照(BC28_SKIP_NSOCO_URC = 1): 收到 OK 就返回成功 —— 复现厂家
  *   BC28_ConTCP() 的写法(那个文件里没有 "+NSOCO" 这个串)。
  */
int BC28_Connect(int sock, const char *ip, uint16_t port, uint32_t to_ms)
{
    char cmd[64];
    int  rc;

    if ((ip == NULL) || (sock < 0))
    {
        return BC28_E_ARG;
    }

    sock_closed = 0;
    nsoco_seen  = 0;                    /* 从现在开始量 "OK 到 URC" 这段 */
    nsoco_ms    = 0;
    nsoco_t0    = BC28_Millis();

    sprintf(cmd, "AT+NSOCO=%d,%s,%u", sock, ip, (unsigned)port);
    rc = at_cmd(cmd, "OK", BC28_TO_LONG, line, sizeof(line));
    if (rc < 0)
    {
        nsoco_seen = 1;                 /* 命令都没受理, 不用再等 URC */
        return rc;
    }

#if (BC28_SKIP_NSOCO_URC != 0)
    /* ---- 复现厂家的写法: 收到 OK 就当连上了 ---- */
    BC28_Idle(BC28_SKIP_WAIT_MS);
    return 0;
#else
    /* ---- 正常写法: 等那条 URC ---- */
    sprintf(cmd, "+NSOCO: %d", sock);
    return wait_for(cmd, to_ms, NULL, 0);
#endif
}

/**
  * @brief  发一行数据。AT+NSOSD=<sock>,<字节数>,<十六进制>
  *
  *   两个容易错的点, 厂家 BC28_Senddata(uint8_t *len, uint8_t *data) 两处都占了:
  *     - <len> 是**原始字节数**, 十六进制字符数是它的两倍(18 字节 -> 36 个字符);
  *     - 数据要**自己转成十六进制**, 模块不认 ASCII 明文。
  *   厂家那版把 len 当指针、用 %s 打, 而且不做十六进制转换。
  */
int BC28_Send(int sock, const char *buf, uint16_t len)
{
    static char        cmd[BC28_TXMAX * 2 + 32];    /* static: 栈只有 1KB */
    static const char  HEX[] = "0123456789ABCDEF";
    uint16_t           i;
    uint16_t           n = 0;
    int                rc;
    int                echo = -1;
    const char        *p;

    if ((buf == NULL) || (len > BC28_TXMAX))
    {
        return BC28_E_ARG;
    }

    n = (uint16_t)sprintf(cmd, "AT+NSOSD=%d,%u,", sock, (unsigned)len);
    for (i = 0; i < len; i++)
    {
        cmd[n++] = HEX[((unsigned char)buf[i] >> 4) & 0x0F];
        cmd[n++] = HEX[(unsigned char)buf[i] & 0x0F];
    }
    cmd[n] = '\0';

    rc = at_cmd(cmd, "OK", BC28_TO_LONG, line, sizeof(line));
    if (rc < 0)
    {
        return rc;
    }

    /* 回显是 "<sock>,<len>"。对不上说明这条没进到我们要的那个 socket 里。 */
    p = seek_num(line);
    if ((p != NULL) && (take_num(p, &echo) == 0) && (echo != sock))
    {
        return BC28_E_PARSE;
    }
    return 0;
}

/**
  * @brief  在响应里找 NSORF 的数据行, 返回该行起点; 找不到返回 NULL。
  *
  *  ⚠ 这是第 3 篇实测踩出来的第二个坑, 比第一个更隐蔽:
  *    手册**表格标题**写的是 "+NSORF:<socket>,<ip>,<port>,<length>,<data>,<remaining>",
  *    但**正文的 Example 和真机实测都是不带前缀的**:
  *         AT+NSORF=2,100
  *         2,203.0.113.7,9101,8,41555448204F4B0A,0      <- 直接就是六个字段
  *         OK
  *    第一版按手册的表格标题去 strstr("+NSORF:"), 于是每次都认成"这次没有数据",
  *    而数据其实一直在模块里躺着。现象是"上行通了、服务端收到了、服务端也回了、
  *    模块也 ACK 了, 但固件永远说 no reply" —— 单看现象根本猜不到是差一个前缀。
  *
  *  这里按行扫, 认**第一个字段等于 sock** 的那行(URC 都以 '+' 开头, 不会误判)。
  */
static const char *nsorf_line(const char *s, int sock)
{
    char        want[8];
    const char *p = s;
    size_t      n;

    sprintf(want, "%d,", sock);
    n = strlen(want);

    while ((p != NULL) && (*p != '\0'))
    {
        if ((strncmp(p, want, n) == 0) &&
            (field(p, 3) != NULL) && (field(p, 4) != NULL))
        {
            return p;
        }
        p = strchr(p, '\n');
        if (p != NULL)
        {
            p++;
        }
    }
    return NULL;
}

/**
  * @brief  收一次。返回收到的字节数(0 = 这次没有新数据), < 0 = 错误码。
  *         AT+NSORF 要**轮询**, 别指望第一条就有 —— 第 2 篇实测的节奏是
  *         每 3~5 秒轮一次、连轮 4~5 次。这里单次超时 5 秒, 轮几次交给调用方。
  */
int BC28_Recv(int sock, char *out, uint16_t cap, uint32_t to_ms)
{
    char        cmd[32];
    const char *p;
    const char *q;
    int         rc;
    int         len = 0;
    uint16_t    need;
    uint16_t    i;

    if ((out == NULL) || (cap == 0) || (sock < 0))
    {
        return BC28_E_ARG;
    }

    need = (cap > BC28_TXMAX) ? BC28_TXMAX : cap;
    sprintf(cmd, "AT+NSORF=%d,%u", sock, (unsigned)need);

    rc = at_cmd(cmd, "OK", to_ms, line, sizeof(line));
    if (rc < 0)
    {
        return rc;
    }

    /* 找数据行: 带 "+NSORF:" 前缀的认, 不带前缀的也认(实测就没前缀, 见上)。
       两种都没有 = 这次真的没有数据, 不是错误。 */
    p = strstr(line, "+NSORF:");
    if (p != NULL)
    {
        p += 7;
    }
    else
    {
        p = nsorf_line(line, sock);
    }
    if (p == NULL)
    {
        return 0;
    }
    while (*p == ' ') { p++; }

    /* 第 4 个字段是字节数 */
    q = field(p, 3);
    if ((q == NULL) || (take_num(q, &len) < 0) || (len <= 0))
    {
        return 0;
    }
    if (len > (int)cap)
    {
        len = (int)cap;
    }

    /* 第 5 个字段是十六进制数据 */
    q = field(p, 4);
    if (q == NULL)
    {
        return BC28_E_PARSE;
    }

    for (i = 0; i < (uint16_t)len; i++)
    {
        int hi = hexval(q[0]);
        int lo = hexval(q[1]);

        if ((hi < 0) || (lo < 0))
        {
            return (i > 0) ? (int)i : BC28_E_PARSE;    /* 能解多少算多少 */
        }
        out[i] = (char)((hi << 4) | lo);
        q += 2;
    }
    if ((uint16_t)len < cap)
    {
        out[len] = '\0';
    }
    return len;
}

/** @brief  关 socket。已经关掉的 socket 再关一次模块回 ERROR, 无害。 */
int BC28_Close(int sock)
{
    char cmd[24];

    if (sock < 0)
    {
        return BC28_E_ARG;
    }
    sprintf(cmd, "AT+NSOCL=%d", sock);
    return at_cmd(cmd, "OK", BC28_TO_LONG, NULL, 0);
}
