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

/* ★ socket 账本 (2026-09-17)。模块一共只给 6 个 socket, **漏光就彻底卡死**:
   AT+NSOCR 直接回 ERROR, 连 AT+NSOCL 都被拒(模块不肯关一个它自己认为
   还占着的号), 这时候剩下的唯一出路是 NRB。详见 BC28_Reset 上面那段注释。

   卡死的可怕之处在于它**自我加速** —— 一次失败制造下一次失败, 所以板子
   会在几分钟内从"能用"掉到"全哑", 而之前能安稳跑几小时。要治它, 先得
   在它发生的时候看得见, 所以这里自己数:
      开 = BC28_Open() 真分配到了号(模块回的是 "<n>\r\nOK")
      关 = BC28_Close() 真收到 OK(回 ERROR 或被超时都不算 —— 那正是漏的那一下)
   差额持续不为 0 就是泄漏, 而且能**在漏光之前**就报出来。 */
static uint32_t sock_open_n;
static uint32_t sock_close_n;

/* ★ +NSOCLI 里那个**号** (2026-09-17 晚上加的)。原来只记了一个布尔
   (sock_closed), 等于把这条 URC 最有用的信息扔了。

   为什么要它 —— 当天晚上的串口实录里, NSOCO 超时的 raw 转储长这样:
       raw<\n\n\n\n+NSOCLI: 1\n\n\n\n+NSOCLI: 5\n\n>
   也就是**我们这边以为关掉的号, 模块几十秒后才真的关**。这就是泄漏的
   机制候选: AT+NSOCL 的 OK 只是"受理", 而 socket 正当 TCP 重传时会拖到
   最后才真释放。号池一共 6 个, 拖久了就撞满。

   有了这两个数, 就能把"猜"换成"看": 关上之后紧接着来了 +NSOCLI 的,
   就是拖后腿的那些; 而 sock_cli_last 一直不出现, 才是真泄漏。 */
static int       sock_cli_last = -1;   /* 最近一次 +NSOCLI 报的号, -1 = 还没有过 */
static uint32_t  sock_cli_cnt;         /* 收到过几次 +NSOCLI */
static uint16_t  sock_cli_pos = 0xFFFF;/* 上一次计数的那个 +NSOCLI 在缓冲里的位置 */

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

/* STOP 低功耗那一套(第四节)。变量放这儿是因为 BC28_Init() 要用 —— 第四节
   的正文在 BC28_Init 后面, 摆在那儿编译器会说"没定义"(第一版就是这么报的)。 */
static uint8_t  rtc_ok;         /* 0=没有可用唤醒源(不进 STOP) 1=LSE 2=LSI */
static uint32_t sleep_cnt;      /* 进过几次 STOP */
static uint32_t sleep_ms_sum;   /* 累计睡掉多少毫秒 */

/* ★ 唤醒定时器**每秒跳几个数**, 定点 Q10(即真实值 x1024)。
   为什么需要它: 这块板子**没有 32.768kHz 晶振**(原理图上 PC14/PC15 是当
   普通 IO 引出来的, 全图只有 8M 的 Y1 和 CH340 的 12M 的 Y2), 所以 RTC 只能
   用 LSI。而 RTC_PRER 的复位值(127x256)是**照 32768Hz 写死的** —— 用 LSE 时
   它正好 1Hz, 换成 LSI 就不是了。实测: 装 25 秒真睡 ~20.8 秒, 比值 1.20,
   即这颗 LSI 约 39.3kHz, 而 CK_SPRE 实际是 1.20Hz 不是 1Hz。
   → "秒"从一开始就不是秒。后果是 UP= 比墙上时钟快 20%, 看板的 boot 会漂,
     而且服务端要 300 秒、实际只等 250 秒, NB-IoT 成本白多 20%。
   所以开机先在**醒着的时候**量一次(见 rtc_calibrate), 拿这个系数换算。
   LSI 本身公差极宽(手册 26~56kHz)且随温度走, 固定预分频挡不住, 只能实测。 */
static uint32_t wut_tps_q10 = 1024UL;   /* 兜底 1.0, 真值开机必被改了 */

/* 唤醒中断真的进过没有(r-1 见 rtc_calibrate)。BC28_WutClear() 里置 1。 */
static volatile uint8_t wut_fired;

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
static void clk_to_32m(void);
static uint8_t rtc_init(void);
static uint8_t rtc_calibrate(void);
static void rtc_dump(const char *why);
static int  sleep_arm(uint32_t secs);
static void wake_check(void);

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
    char *p = strstr(rx + rx_tail, "+NSOCLI");

    if (p != NULL)
    {
        sock_closed = 1;

        /* ★ 只在**位置变了**的时候才解析+计数。

           原因: BC28_Idle() 每 20 毫秒调一次 urc_scan(), 而它**不清缓冲**
           (见 BC28_Idle 里那段 "每圈都 strstr 一遍 1KB 是白费")。于是一段
           +NSOCLI 只要还在缓冲里, 每秒会被反复扫到约 50 次 ——
           布尔 sock_closed 无所谓(重复置 1 还是 1), 但计数器会疯涨:
           2026-09-18 实测跑了 1 小时, sock_cli_cnt 涨到 **719 万**。

           判据 "这次找到的位置 != 上次找到的位置" 就够了。rx_drop_all() 会把它
           复位成 0xFFFF(缓冲清空后同一个偏移可以是新的一条 URC)。
           ⚠ 这是个**近似**: 缓冲如果被搬移(紧凑化), 偏移会变, 可能多算一次。
           对"看趋势"够用, 别拿它当精确计数用。 */
        if ((uint16_t)(p - rx) != sock_cli_pos)
        {
            char *q = p + 7;            /* 跳过 "+NSOCLI" */

            /* 顺手把号也记下来 (见 sock_cli_last 那段)。号只有 0~5(模块一共 6 个),
               所以一位十进制就够。

               ⚠ 冒号后面那个**空格不能省着不跳** —— 第一版就是栽在这里: 实际字节是
               "+NSOCLI: 1"(AT 标准格式, 冒号后带空格), 于是判据不成立, 号一个都
               没记上 —— 而 sock_closed 照样置 1, 所以"能看见 NSOCLI 来了、却看不见
               是哪个号", 从日志上根本看不出是解析错。修法就是老老实实跳空格。

               ★★ 第二版又栽了一次 (2026-09-18 23:46 实测): 计数变成**永远是 0**。
                  原因是 `sock_cli_pos` 在**进这个块的第一句**就被消费掉了, 而那条
                  URC 是一字节一字节收进来的 —— 第一次扫到时缓冲里只有
                  `\n\n\n\n+NSOCLI`(**数字还没到**), 位置记成 4 但没解析出号;
                  下一圈数字到了、位置**还是 4**, 于是被 `p - rx != sock_cli_pos`
                  判成"扫过了"直接跳过。**位置票被一张没用的行程用掉了。**
                  指纹: `[sock] ... nsocli=-1/0`, 而同一时刻的 `raw<>` 里明明有
                  `+NSOCLI: 3` —— 和第一版是同一副面孔, 根因完全不同。

                  修法: **位置只在真解析出号之后才消费**, 半截行不算数。 */
            if (*q == ':')
            {
                q++;
            }
            while ((*q == ' ') || (*q == '\t'))
            {
                q++;
            }
            if ((*q >= '0') && (*q <= '9'))
            {
                sock_cli_last = (int)(*q - '0');
                sock_cli_cnt++;
                sock_cli_pos = (uint16_t)(p - rx);   /* ← 只有这一次才"消费位置" */
            }
        }
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
    sock_cli_pos = 0xFFFF;      /* 缓冲清了, 偏移重新算 —— 见 urc_scan 里那段 */
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

    /* ★ NRB 会把模块**所有** socket 一次性放掉, 所以账本必须跟着清零。
       不清的后果很具体: 主循环那个"余额 >= 4 就主动 NRB"的保险会**每轮都触发**,
       板子就变成隔一轮重启一次, 比泄漏本身还糟。

       顺序上放在 NRB **之前** —— 万一 NRB 那句本身超时失败了(它本来就可能收不到
       OK), 我们也不能留着旧账, 否则同样会误判。 */
    sock_open_n  = 0;
    sock_close_n = 0;

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

    /* RTC 唤醒源。**必须在 tim4_init 之后**(它自己不用 TIM4, 但后面睡觉得
       靠 BC28_Millis 判退出)。起来就用它; 起不来就置 rtc_ok=0,
       往后所有 sleep 自动退回空转 —— 见文件里"四、STOP 低功耗"那一段。
       ★ 唤醒定时器的 WUTR 只在 WUTE=0 时写得进去, 所以初始化里就把它关着。 */
    rtc_ok   = rtc_init();
    sleep_cnt    = 0;
    sleep_ms_sum = 0;

    /* 复位原因。RCC_CSR 高 8 位是**上次为什么重启**, 上电后第一次读有效。
       为什么值得打: 2026-09-16 实测遇到过一次"跑着跑着自己重启了", 而串口上
       什么也没留下 —— 有这一行就能当场分清是掉电(POR)、软件复位(SFT, 那基本
       就是 HardFault 处理器干的)、还是低功耗复位(LPWR)。没有它就是纯猜。 */
    printf("+ reset cause:%s%s%s%s%s\r\n",
           (RCC->CSR & RCC_CSR_PORRSTF)   ? " POR"   : "",
           (RCC->CSR & RCC_CSR_PINRSTF)   ? " PIN"   : "",
           (RCC->CSR & RCC_CSR_SFTRSTF)   ? " SOFTWARE" : "",
           (RCC->CSR & RCC_CSR_IWDGRSTF)  ? " IWDG"  : "",
           (RCC->CSR & RCC_CSR_LPWRRSTF)  ? " LOWPOWER" : "");
    RCC->CSR |= (uint32_t)RCC_CSR_RMVF;      /* 清掉, 免得下次读到的是这一次 */

    if (rtc_ok == 0)
    {
        printf("! RTC wakeup unavailable -- STOP disabled, will busy-wait\r\n");
    }
    else
    {
        /* ★ 打两个数, 不是一个:
           tps_q10 是**标定出来的**每秒跳几个数(1024 = 1.0)。这块板子没晶振,
           ck_spre 不是 1Hz, 所以这个数几乎不可能正好是 1024 —— 它就是那个
           20% 误差的直接证据, 也是 sleep_arm 折算时用的系数。
           `ms/1e3` 再换算成"每个数几毫秒"更好读。 */
        printf("+ RTC wakeup ready on %s (cal: %lu.%03lu ticks/s,"
               " i.e. %lu.%02lu ms/tick)\r\n",
               (rtc_ok == 1) ? "LSE" : "LSI",
               (unsigned long)(wut_tps_q10 / 1024UL),
               (unsigned long)(((wut_tps_q10 % 1024UL) * 1000UL) / 1024UL),
               (unsigned long)(1024UL * 1000UL / wut_tps_q10),
               (unsigned long)(((1024UL * 1000UL) % wut_tps_q10) * 100UL
                               / wut_tps_q10));
    }

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

/* =========================================================================
 * 四、STOP 低功耗
 *
 *  为什么要它: 实测这块板 30 秒一报时平均 15~20mA, 1000mAh 电池只够 2~3 天。
 *  省电有三个台阶: **降频**(改 report_ms, 零风险) -> **MCU 进 STOP**(这一节)
 *  -> **模块 PSM**(最后一节)。这一节只做中间那个。
 *
 *  ★ 为什么必须"板子下次上报时顺便拉配置"而不是服务端推:
 *    进了 STOP 就**收不到字节** —— USART2 的时钟停了, 而 BC28_Init() 里
 *    本来就把 RXNE 中断关掉了(要自己轮询 DR)。所以"随时能收命令"跟"省电"
 *    在 NB-IoT 上天生互斥, 拉取是唯一自洽的做法。
 *
 *  ★ 底线: **RTC 唤醒源起不来就绝不进 STOP**, 自动退回 BC28_Idle 空转。
 *    宁可费电, 不能睡死 —— 睡死的板子要人到现场拔电。
 * ========================================================================= */

/* 等 HSE/PLL/VOS 就绪的上限。⚠ 量的是**圈数**不是毫秒: 这几段代码跑的时钟
   本身就是待定的(刚退 STOP 时可能是 HSI 16MHz, 也可能还落在 MSI 2.1MHz),
   拿毫秒去卡反而会算错。给足圈数, 最慢那种情况(2.1MHz)下也在 0.5 秒量级。 */
#define CLK_GUARD       200000UL

/* 醒来探活连发几次 AT。模块是跟我们同时醒的, UART 上头一两个字符常丢 ——
   **要容忍前两次失败**, 不能一次不应就判死。 */
#define BC28_WAKE_TRIES 3

/* 标定唤醒定时器时倒数几个数(名义"秒")。取 2 的理由:
   够长, LSI 那么宽的误差下 2 个数也能量到 ~1% 的分辨率;
   够短, 开机多花 ~2 秒, 比起"UP= 漂 20%"完全值得。 */
#define RTC_CAL_TICKS   2UL

/* 要不要试着用 LSE(外接 32.768kHz 晶振)当 RTC 时钟。**默认 0。**
   本板原理图上没有这颗晶振 —— 全图只有 Y1(8M/HSE)和 Y2(12M/CH340),
   PC14-OSC32_IN / PC15-OSC32_OUT 是当普通 IO 网标(STM32_PC14/PC15)引出去的,
   连负载电容都没有。所以配 LSEON 去驱动那对引脚本身就是手册不允许的状态,
   会报出一个**假的 LSERDY=1** —— 而 RTCSEL 只能写一次, 一旦被这个假信号
   骗着锁成 LSE, 就再没有可用的唤醒源了(这是永久性的, 要去现场拔电池)。
   → 既然要的是 LSI, 就别把 LSE 打开。
   真有晶振的板子把这个改成 1, 但**先把上面那句话读第二遍**。 */
#define BC28_RTC_TRY_LSE   0

/* (rtc_ok / sleep_cnt / sleep_ms_sum 和这四个函数的原型都在文件顶部 ——
   BC28_Init() 在第四节**之前**, 要用到它们。) */

/**
  * @brief  把系统时钟重建到 32MHz。**退 STOP 之后必须先做这一步。**
  *
  *  STOP 会把 Vcore 域所有时钟停掉(HSE、PLL 全关), 醒来跑在一个很低的时钟上
  *  (HSI 16MHz 或 MSI 2.1MHz)。不重建的话:
  *    USART2 的分频是照着 32MHz 算的 -> 波特率腰斩 -> 跟模块完全说不上话;
  *    TIM4 的预分频也是照着 32MHz 算的 -> 毫秒时基变成两倍慢;
  *    DHT11 的 TIM6 微秒时基同理 -> 传感器直接读不出。
  *  **三样一起坏, 现象是"板子哑了", 极容易误判成模块坏了。**
  *
  *  抄的是 system_stm32l1xx.c 的 SetSysClock() —— 它是 static, 外部调不到,
  *  只能自己写一份。两处**故意不一样**:
  *
  *   (1) 所有 while 都带上限。SetSysClock 是上电跑的, 那时卡住还能看出来;
  *       这里是睡醒跑的, 卡住就是**板子从此静默**。
  *
  *   (2) ★★ 电压那一步**只改 VOS 两位, 不整写 PWR->CR**。
  *       SetSysClock 里是 `PWR->CR = PWR_CR_VOS_0;` —— 整写。上电时 DBP=0
  *       无所谓; 但退 STOP 时 DBP 是 1(我们解开了备份域才能写 RTC), 整写
  *       会把它清掉, 于是**下一次** sleep 的 WUTR 写不进去。而且是**静默失败**:
  *       第一次睡得好好的, 第二次再也醒不过来 —— 不报错、不 HardFault,
  *       就是安静地不再说话。这是整个功能里最阴的一个坑。
  *
  *  ★ HSE 起不来的兜底: 改用 HSI。16MHz x6 /3 = 32MHz —— **频率一模一样**,
  *    所以 SystemCoreClock、UART 分频、TIM4 预分频一个字都不用改。
  *    (要是退回 HSI 16MHz 而不是拉到 32MHz, 上面那三样还是会全错。)
  */
static void clk_to_32m(void)
{
    uint32_t guard;
    uint32_t hse_ok;
    uint32_t cr;

    RCC->APB1ENR |= (uint32_t)RCC_APB1ENR_PWREN;   /* 下面要写 PWR->CR */

    /* ---- 1. 重开 HSE(STOP 期间它是停的) ---- */
    RCC->CR |= (uint32_t)RCC_CR_HSEON;
    guard = 0;
    while (((RCC->CR & RCC_CR_HSERDY) == 0) && (guard < CLK_GUARD))
    {
        guard++;
    }
    hse_ok = (RCC->CR & RCC_CR_HSERDY) ? 1u : 0u;

    /* ---- 2. 先切到 HSI 再关 PLL。PLL 当着系统时钟是不能关的 ---- */
    RCC->CR |= (uint32_t)RCC_CR_HSION;
    guard = 0;
    while (((RCC->CR & RCC_CR_HSIRDY) == 0) && (guard < CLK_GUARD))
    {
        guard++;
    }

    RCC->CFGR &= ~(uint32_t)RCC_CFGR_SW;
    guard = 0;
    while (((RCC->CFGR & RCC_CFGR_SWS) != 0) && (guard < CLK_GUARD))
    {
        guard++;
    }

    RCC->CR &= ~(uint32_t)RCC_CR_PLLON;
    guard = 0;
    while (((RCC->CR & RCC_CR_PLLRDY) != 0) && (guard < CLK_GUARD))
    {
        guard++;
    }

    /* ---- 3. Flash: 64 位访问 + 预取 + 1 个等待周期 ----
       0 等待周期只能跑到 16MHz, 32MHz 必须要 1 个。漏了这条不是"慢一点",
       是取指偶尔读到半截, 表现为随机死机。 */
    FLASH->ACR |= (uint32_t)FLASH_ACR_ACC64;
    FLASH->ACR |= (uint32_t)FLASH_ACR_PRFTEN;
    FLASH->ACR |= (uint32_t)FLASH_ACR_LATENCY;

    /* ---- 4. 电压范围 1 (1.8V)。32MHz 只在 Range1 下允许。 ----
       ★★ 只动 VOS 两位 + 把 DBP 保回来, 见函数头 (2)。 */
    cr  = PWR->CR;
    cr &= ~(uint32_t)(PWR_CR_VOS_0 | PWR_CR_VOS_1);
    cr |=  (uint32_t)PWR_CR_VOS_0;
    cr |=  (uint32_t)PWR_CR_DBP;
    PWR->CR = cr;

    guard = 0;
    while (((PWR->CSR & PWR_CSR_VOSF) != 0) && (guard < CLK_GUARD))
    {
        guard++;
    }

    /* ---- 5. HCLK / PCLK1 / PCLK2 都是 SYSCLK 不分频 ---- */
    RCC->CFGR |= (uint32_t)RCC_CFGR_HPRE_DIV1;
    RCC->CFGR |= (uint32_t)RCC_CFGR_PPRE2_DIV1;
    RCC->CFGR |= (uint32_t)RCC_CFGR_PPRE1_DIV1;

    /* ---- 6. PLL: 两条路都精确落在 32MHz ----
         HSE 8MHz x12 /3 = 32MHz   (VCO 96MHz)
         HSI 16MHz x6 /3 = 32MHz   (VCO 96MHz, 同一个值) */
    RCC->CFGR &= ~(uint32_t)(RCC_CFGR_PLLSRC | RCC_CFGR_PLLMUL | RCC_CFGR_PLLDIV);
    if (hse_ok)
    {
        RCC->CFGR |= (uint32_t)(RCC_CFGR_PLLSRC_HSE | RCC_CFGR_PLLMUL12 |
                                RCC_CFGR_PLLDIV3);
    }
    else
    {
        RCC->CFGR |= (uint32_t)(RCC_CFGR_PLLMUL6 | RCC_CFGR_PLLDIV3);
    }

    RCC->CR |= (uint32_t)RCC_CR_PLLON;
    guard = 0;
    while (((RCC->CR & RCC_CR_PLLRDY) == 0) && (guard < CLK_GUARD))
    {
        guard++;
    }

    RCC->CFGR &= ~(uint32_t)RCC_CFGR_SW;
    RCC->CFGR |=  (uint32_t)RCC_CFGR_SW_PLL;
    guard = 0;
    while (((RCC->CFGR & RCC_CFGR_SWS) != (uint32_t)RCC_CFGR_SWS_PLL) &&
           (guard < CLK_GUARD))
    {
        guard++;
    }

    /* HSI 那条路是可用的, 但**不正常**, 每次都喊一声。
       必须在这句时钟重建**之后**打印 —— 之前打印的话波特率还是错的, 打出来是乱码。 */
    if (hse_ok == 0)
    {
        printf("! HSE did not come back after STOP -- running on HSI x6/3"
               " (still 32 MHz, UART/timebase unaffected)\r\n");
    }
}

/**
  * @brief  清掉 WUTF 和 EXTI 线 20 的挂起位。**睡下去还能不能醒, 全靠这一下。**
  *
  *  ★ 这里**显式解开 WPR** 再清, 而库里的 RTC_ClearFlag() / RTC_ClearITPendingBit()
  *    自己不解。理由: 如果 ISR 里这几位其实受 WPR 保护, 那两次清就都是
  *    **静默失败**(不报错, 位就是没清掉), 后果是 EXTI 线上再也形不成上升沿 ——
  *    **板子睡下去就再也醒不过来**, 而且没有任何先兆。
  *    最坏情况按"要解"写: 多解一次是幂等的, 少解一次是去现场拔电。
  *
  *  EXTI 那个挂起位是普通寄存器, 本来就好清; 但 WUTF 必须清 —— 不清的话
  *  下一次 WFI 会**立刻返回**, 进不去 STOP("睡了等于没睡")。
  */
void BC28_WutClear(void)
{
    RTC->WPR = 0xCA;
    RTC->WPR = 0x53;

    /* 只清 WUTF 这一位, 其余原样写回。ISR 里的只读位(WUTWF/INITF/RSF 等)
       写回去会被硬件忽略, TSF/TSOVF 那些本工程没用到。 */
    RTC->ISR &= ~(uint32_t)RTC_ISR_WUTF;

    EXTI_ClearITPendingBit(EXTI_Line20);

    RTC->WPR = 0xFF;

    /* ★ 告诉 rtc_calibrate() "中断真的进来了"。
       它等的是这个变量而**不是** WUTF 标志位 —— WUTF 在这里刚被清掉, 两边
       同时盯着同一个标志位会互相把现象吃掉(ISR 先跑就把标志清了, 那边永远
       等不到)。而这个变量是 ISR 唯一留下的痕迹, 等它等于把
       RTC→EXTI→NVIC→入口 整条链都验过了 —— 那正是"睡下去醒不过来"的唯一入口。
       置 1 是幂等的: 平时睡醒也会走到这儿, 没人读, 无副作用。 */
    wut_fired = 1;
}

/**
  * @brief  把 RTC 的唤醒定时器配起来。**整个 STOP 功能的前提。**
  *
  *  用哪个时钟源**以 RTCSEL 寄存器为准**(它只能写一次, 是既成事实),
  *  本板实际落到 LSI(内部 RC)。LSI 原先标称 37kHz 但公差宽到 ±30%,
  *  而 RTC_PRER 的复位值是照 32768Hz 写死的 → ck_spre 不是 1Hz。
  *  所以**开机必须标定**(rtc_calibrate)并打印结果, 不能假装它是秒。
  *
  *  返回 0 = 没有可用唤醒源(调用方**绝不能进 STOP**); 1 = LSE; 2 = LSI。
  *
  *  ★ 用的是 **WakeUp Timer 不是 Alarm**: 纯周期倒数、auto-reload,
  *    不用维护 BCD 日历也不用每轮重设。
  *
  *  ★ 写 RTC 寄存器前必须先解备份域写保护(PWR->CR 的 DBP), 否则所有写
  *    都是**静默失败** —— 不报错, 值就是没进去。
  *
  *  ★ 这里的写操作**故意不用 RCC_LSEConfig()**: 那个函数是整字节写
  *    RCC->CSR 的高字节(CSR_BYTE2_ADDRESS), 会把同一个字节里的
  *    RTCSEL/RTCEN 一起清零。而 RTCSEL 在**备份域复位之后只能写一次** ——
  *    这块板有锂电管理, 备份域很可能活着(甚至跨断电), 那这次清零就是永久性的,
  *    RTC 从此没有时钟。所以下面一律只置位、不整写那个字节。
  */
static uint8_t rtc_init(void)
{
    uint32_t t0;
    uint32_t sel;
    uint8_t  src;

    RCC->APB1ENR |= (uint32_t)RCC_APB1ENR_PWREN;
    PWR_RTCAccessCmd(ENABLE);            /* DBP = 1, 不解开后面全是静默失败 */

    /* ---- 1. LSI **无条件**打开, 不等任何条件 ----
       ★★ 这一句的位置是 2026-09-16 修 bug 改出来的, 别挪回 if 里面去。
       原来写成"LSE 起不来才开 LSI"(if (src == 0) 里面), 看着省事, 实际埋了雷:
       LSEON 一直被置着(本文件从不清它), 而这块板子 PC14/PC15 按原理图是当
       **普通 IO** 引到底板的 —— 于是 LSE 那条路某一刻报出了 LSERDY=1, src 成了 1,
       **LSI 那段整个被跳过**。可 RTCSEL 早被烧死成 LSI(只能写一次),
       RCC_RTCCLKConfig(LSE) 是空操作, 结果就是:
         RTCSEL=LSI + LSION=0 + RTCEN=1  →   RTC 被一个**关掉的**振荡器供着。
       现象: 同一份固件有的开机能睡(rtc=2)、有的开机报 rtc=0, 寄存器上看不出
       差别的"玄学"。实测 CSR=04420300 就是这么读出来的。
       教训: **判据不是"LSE 起没起来", 是"RTCSEL 最终选中的那个源必须开着且就绪"。**
       所以 LSI 先无条件开 —— 它一定在, 且是本板真正要用的那个。 */
    RCC->CSR |= (uint32_t)RCC_CSR_LSION;
    t0 = BC28_Millis();
    while (((RCC->CSR & RCC_CSR_LSIRDY) == 0) &&
           ((uint32_t)(BC28_Millis() - t0) < 500UL))
    {
    }

    /* ---- 2. LSE 要不要试, 由编译开关说了算 ----
       默认 0(不试)。理由: 本板原理图上**没有 32.768kHz 晶振** —— 全图只有
       Y1(8M, HSE)和 Y2(12M, CH340), PC14-OSC32_IN / PC15-OSC32_OUT 是当普通
       IO 网标(STM32_PC14/PC15)引到底板的, 连负载电容都没有。
       而且置着 LSEON 去驱动一对被当 GPIO 用的引脚, 本身就是手册不允许的状态,
       那个假的 LSERDY=1 就是这么来的。既然选 LSI, 就别再把 LSE 打开。
       (有晶振的板子把这个宏改成 1 即可; 但**先想清楚** RTCSEL 只能写一次。) */
#if BC28_RTC_TRY_LSE
    RCC->CSR |= (uint32_t)RCC_CSR_LSEON;
    t0 = BC28_Millis();
    /* 用 BC28_Millis 量毫秒而不是循环圈数: 32.768kHz 晶振起振是**秒级**的
       (典型几百毫秒), 拿圈数去卡根本等不到 —— 现象是"有晶振却总报 LSI"。 */
    while (((RCC->CSR & RCC_CSR_LSERDY) == 0) &&
           ((uint32_t)(BC28_Millis() - t0) < 3000UL))
    {
    }
#endif

    /* ---- 3. RTCSEL 是**既成事实**, 以它为准, 不听我们想选谁 ----
       RTCSEL 在备份域复位之后只能写一次。这块板有锂电管理, 备份域很可能活着
       (甚至跨断电), 所以它很可能早就被烧死了 —— 那这次写就是不生效的。
       先读它, 才知道该保证哪个源开着。 */
    sel = RCC->CSR & RCC_CSR_RTCSEL;

    if (sel == RCC_CSR_RTCSEL_NOCLOCK)
    {
        /* 备份域是新的, 这一次写才有效。选 LSI(见上面 #if 的理由)。 */
#if BC28_RTC_TRY_LSE
        src = ((RCC->CSR & RCC_CSR_LSERDY) != 0) ? 1u : 2u;
#else
        src = 2u;
#endif
        if ((src == 1) && ((RCC->CSR & RCC_CSR_LSERDY) == 0)) { src = 2u; }
        RCC_RTCCLKConfig((src == 1) ? RCC_RTCCLKSource_LSE : RCC_RTCCLKSource_LSI);
    }
    else if (sel == RCC_CSR_RTCSEL_LSE) { src = 1u; }
    else if (sel == RCC_CSR_RTCSEL_LSI) { src = 2u; }
    else
    {
        /* RTCSEL = HSE/RTCPRE —— 本工程从没这么配过, 也不打算用。
           真碰上只有可能是别人烧进去的, 这里不敢接着用。 */
        rtc_dump("RTCSEL is HSE/RTCPRE (not ours)");
        return 0;
    }

    RCC_RTCCLKCmd(ENABLE);

    /* ---- 4. 该开的开到了没有: 只认 RTCSEL 选中的那一个 ----
       (不选 LSE 时就不开 LSE —— 既是省那 1uA, 也是别让它去跟当 GPIO 用的
        PC14/PC15 打架。) */
    if (src == 1u)
    {
        if ((RCC->CSR & RCC_CSR_LSERDY) == 0) { rtc_dump("need LSE but it is not up"); return 0; }
    }
    else
    {
        if ((RCC->CSR & RCC_CSR_LSIRDY) == 0) { rtc_dump("need LSI but it is not up"); return 0; }
    }

    /* ---- 唤醒定时器: ck_spre 基准, 16 位, 最长 65536 个数 ---- */
    if (RTC_WakeUpCmd(DISABLE) != SUCCESS)
    {
        rtc_dump("WUTWF never set (wakeup timer will not release)");
        return 0;
    }
    RTC_WakeUpClockConfig(RTC_WakeUpClock_CK_SPRE_16bits);

    /* 中断这条路必须整条打通, 缺一段就是"睡下去醒不过来":
       RTC 的 WUT 事件 -> EXTI 线 20 -> NVIC 的 RTC_WKUP_IRQn -> 那个处理函数。
       (startup 里 RTC_WKUP_IRQHandler 是**弱符号**, 指向 `B .` 死循环 ——
        在 stm32l1xx_it.c 里不定义它, 一唤醒就跳进死循环, 这是最容易漏、
        后果最严重的一处。) */
    EXTI_ClearITPendingBit(EXTI_Line20);
    RTC_ClearFlag(RTC_FLAG_WUTF);
    BC28_WutClear();
    RTC_ITConfig(RTC_IT_WUT, ENABLE);
    {
        EXTI_InitTypeDef ei;
        EXTI_StructInit(&ei);
        ei.EXTI_Line    = EXTI_Line20;
        ei.EXTI_Mode    = EXTI_Mode_Interrupt;
        ei.EXTI_Trigger = EXTI_Trigger_Rising;
        ei.EXTI_LineCmd = ENABLE;
        EXTI_Init(&ei);
    }
    NVIC_SetPriority(RTC_WKUP_IRQn, 1);
    NVIC_EnableIRQ(RTC_WKUP_IRQn);

    /* ---- ★★ 最后一关, 也是唯一真正算数的一关: **当场醒一次** ----
       上面那些全是"愿不愿意"的检查(寄存器说 RTCSEL 是 LSI、RDY 置位了),
       可"寄存器看着对"跟"这块板子能醒过来"是两件事 —— 2026-09-16 实测就撞上了:
       同一份固件, 有的开机能起来(rtc=2 睡得好好的), 有的开机报 rtc=0,
       而成功的路径和失败的路径在寄存器上找不出差别。

       所以改成功能验证: 按标称装一次倒数, 真的等那根中断线把 wut_fired 置起来。
       这一下同时验了 WUT 计数 / EXTI 线 20 / NVIC / 中断入口 一整条链。
       **醒不过来就返回 0, 绝不进 STOP** —— 宁可费电, 不能睡死。
       (用 BC28_Idle 等, 不能空转 while: 那会把 TIM4 的时基饿停。) */
    if (rtc_calibrate() == 0)
    {
        rtc_dump("wake-up self test failed -- this board CANNOT wake itself");
        return 0;
    }

    return src;
}

/**
  * @brief  挂掉的时候把决定性的那几个寄存器打出来。
  *
  *  为什么值得占这几行: 这些失败路径**原本一个字节都不打** —— 现象是
  *  "开机没看见 RTC wakeup ready", 然后板子照常跑(退回空转), 一切看着正常,
  *  只能靠翻代码猜是哪一条 return 0 命中了。有了 CSR 和 PRER 就能直接读出
  *  时钟源选的是谁、分频比是多少, 不用猜。
  */
static void rtc_dump(const char *why)
{
    printf("! RTC: %s\r\n", why);
    /* RTCSEL 在 CSR 的 bit17:16(不是 bit8) —— 照 CMSIS 头文件核过:
       0x00010000=LSE 0x00020000=LSI 0x00030000=HSE/RTCPRE 0=NOCLOCK。
       这一格是判"备份域里存的到底是哪个源"的唯一依据。 */
    printf("  CSR=%08lX RTCSEL=%lu LSEON=%d LSERDY=%d LSION=%d LSIRDY=%d RTCEN=%d\r\n",
           (unsigned long)RCC->CSR,
           (unsigned long)((RCC->CSR & RCC_CSR_RTCSEL) >> 16),
           (int)((RCC->CSR & RCC_CSR_LSEON)   ? 1 : 0),
           (int)((RCC->CSR & RCC_CSR_LSERDY)  ? 1 : 0),
           (int)((RCC->CSR & RCC_CSR_LSION)   ? 1 : 0),
           (int)((RCC->CSR & RCC_CSR_LSIRDY)  ? 1 : 0),
           (int)((RCC->CSR & RCC_CSR_RTCEN)   ? 1 : 0));
    printf("  PRER=%08lX (A=%lu S=%lu) ISR=%08lX WUTR=%lu tps_q10=%lu\r\n",
           (unsigned long)RTC->PRER,
           (unsigned long)((RTC->PRER >> 16) & 0x7Fu),
           (unsigned long)(RTC->PRER & 0x7FFFUL),
           (unsigned long)RTC->ISR, (unsigned long)RTC->WUTR,
           (unsigned long)wut_tps_q10);
}

/**
  * @brief  ★ 量出唤醒定时器**每秒跳几个数**(Q10 定点), 顺便证明这板子能醒。
  *
  *  为什么必须实测而不是写死 1: 见 wut_tps_q10 的注释 —— 板子没晶振, RTC 走
  *  LSI, 而 PRER 复位值是照 32768Hz 写死的。所以 ck_spre 不是 1Hz, 是
  *  f_LSI/32768(这颗实测约 1.20Hz)。LSI 的公差宽到 ±30%, 写死任何常数都是赌。
  *
  *  做法: 按标称装 RTC_CAL_TICKS 个数, 量真实毫秒数(BC28_Millis 走 TIM4,
  *  源头是 HSE 8MHz, 这个数是准的), 除一下就是每个数几毫秒。
  *
  *  ★★ 量的必须是**第二拍, 不是第一拍**。这是 2026-09-16 实测出来的:
  *     同一份固件连续复位 6 次, 5 次稳定读到 1.277 ticks/s, 有一次跳到 2.394
  *     —— 差 87%。原因不是 LSI 在漂(那 5 次稳到 ±0.2%), 而是 WUT 走的是
  *     ck_spre, ck_spre 由预分频链自由跑出来, **使能那一刻不跟相位对齐**:
  *     头一拍可能早到将近一整个 ck_spre 周期(这颗 783ms, 是标称 1.566s 的一半)。
  *     2.394 反推量到 855ms、1.456 反推 1374ms, 正是"早了一部分相位"。
  *     第一拍之后 WUT 自动重装并跟相位对齐, **两拍之间正好是一个完整周期**,
  *     量它就没有相位误差。
  *
  *  ★ 这里**故意等中断**(wut_fired)而不是轮询 WUTF 标志位: WUTF 是 ISR 清的,
  *  两边同时抢那个标志位会互相把对方的现象吃掉。等 ISR 置的变量, 相当于把
  *  "RTC→EXTI→NVIC→入口"整条链一起验了。而这条链正是"睡下去醒不过来"的
  *  唯一入口 —— 验不过就不许睡。
  */
static uint8_t rtc_calibrate(void)
{
    uint32_t t0;
    uint32_t t1;
    uint32_t ms;
    uint32_t guard;

    /* 标称 RTC_CAL_TICKS 秒, 给到 4 倍还是没动静就是真醒不过来。
       (LSI 最坏 26kHz 也只是慢 1.4 倍, 4 倍足够宽。) */
    guard = (RTC_CAL_TICKS * 1000UL) * 4UL;

    if (RTC_WakeUpCmd(DISABLE) != SUCCESS) { return 0; }
    RTC_SetWakeUpCounter(RTC_CAL_TICKS - 1UL);
    BC28_WutClear();
    wut_fired = 0;
    RTC_WakeUpCmd(ENABLE);

    /* ---- 第一拍: 只等对齐, 时间不采信 ---- */
    t0 = BC28_Millis();
    while (wut_fired == 0)
    {
        if ((uint32_t)(BC28_Millis() - t0) > guard)
        {
            (void)RTC_WakeUpCmd(DISABLE);
            BC28_WutClear();
            return 0;
        }
        BC28_Idle(1);          /* 必须让时基继续走, 不能空转 */
    }

    /* ---- 第二拍: 量这一拍。它跟第一拍之间正好一个完整周期 ---- */
    wut_fired = 0;
    t1 = BC28_Millis();
    while (wut_fired == 0)
    {
        if ((uint32_t)(BC28_Millis() - t1) > guard)
        {
            (void)RTC_WakeUpCmd(DISABLE);
            BC28_WutClear();
            return 0;
        }
        BC28_Idle(1);
    }

    ms = (uint32_t)(BC28_Millis() - t1);
    (void)RTC_WakeUpCmd(DISABLE);
    BC28_WutClear();

    if (ms == 0UL) { return 0; }

    /* ticks/秒 = 数过的个数 / 真实秒数, 乘 1024 存成 Q10。
       数量级: RTC_CAL_TICKS=2, ms≈1566 → 2*1024*1000/1566 = 1308。 */
    wut_tps_q10 = ((uint32_t)RTC_CAL_TICKS * 1024UL * 1000UL) / ms;
    if (wut_tps_q10 < 64UL) { wut_tps_q10 = 64UL; }      /* 0.0625 tps 以下离谱 */
    if (wut_tps_q10 > 4096UL) { wut_tps_q10 = 4096UL; }  /* 4 tps 以上离谱 */
    return 1;
}

/**
  * @brief  给唤醒定时器装上 secs 秒的倒数, 允许它把 MCU 从 STOP 里叫醒。
  *         返回 0 = 装好了; < 0 = 装不上(调用方退回空转)。
  */
static int sleep_arm(uint32_t secs)
{
    uint32_t ticks;

    /* 调用方已经夹过一遍了, 这里是第二道 —— WUTR 只有 16 位, 越界会被
       库里的 assert 拦下(本工程没开 assert, 那就是**静默截断**)。 */
    if (secs < 1UL) { secs = 1UL; }
    if (secs > 65536UL) { secs = 65536UL; }

    /* ★ 秒 -> 数。**这一步不能省**: 唤醒定时器走 ck_spre, 而这块板子没晶振、
       RTC 走 LSI, ck_spre 实测约 1.20Hz 不是 1Hz。直接写 secs-1 的话,
       要 25 秒真睡 ~20.8 秒, 注入时基却按 25 秒算 —— UP= 就比墙上时钟快 20%。
       wut_tps_q10 是开机标定出来的"每秒跳几个数", 见 rtc_calibrate()。
       溢出: secs 最大 65536, tps_q10 ~1230 → 8.1e7, 32 位装得下。 */
    ticks = ((secs * wut_tps_q10) + 512UL) / 1024UL;   /* +512 = 四舍五入 */
    if (ticks < 1UL)      { ticks = 1UL; }
    if (ticks > 65536UL)  { ticks = 65536UL; }

    /* WUTR 只在 WUTE=0 时写得进去, 所以先关。RTC_WakeUpCmd(DISABLE) 内部
       会等 WUTWF(手册要求的那一位)—— 等不到就返回 ERROR, 那说明 RTC 时钟
       根本没在跑, 这时候进 STOP 就再也醒不来了。 */
    if (RTC_WakeUpCmd(DISABLE) != SUCCESS)
    {
        return -1;
    }

    RTC_SetWakeUpCounter(ticks - 1UL);     /* 周期 = WUTR + 1 个数 */

    /* ★ 睡前**必须**把 WUTF 和 EXTI 的挂起位清干净。
       只要还剩一个挂起位, WFI 会**立刻返回**, 根本进不去 STOP。
       现象是"间隔没变、电流也没降", 而代码看上去完全正常, 极难查。 */
    BC28_WutClear();

    RTC_WakeUpCmd(ENABLE);
    return 0;
}

/**
  * @brief  睡醒之后探一下模块还在不在。
  *
  *  模块是跟我们同时醒的, 而且 STOP 期间 USART2 完全没有时钟, 它要是正好
  *  在说话, 那几个字节是**永远丢了**的。所以醒来固定连发 3 次 AT, 容忍前两次失败。
  *
  *  ★ 三次都不应就**当场 AT+NRB**, 不走主循环那套"连续 5 次失败再救" ——
  *    300 秒一轮时那是 **25 分钟**。重启代价十几秒, 而且这一轮本来也发不出去。
  */
static void wake_check(void)
{
    int i;

    for (i = 0; i < BC28_WAKE_TRIES; i++)
    {
        if (at_cmd("AT", "OK", BC28_TO_AT, NULL, 0) == 0)
        {
            return;                      /* 活着。绝大多数情况第一次就过 */
        }
        BC28_Idle(200);
    }

    printf("! BC28 silent after STOP (%d ATs) -- AT+NRB now\r\n", BC28_WAKE_TRIES);
    BC28_Reset();
    (void)BC28_WaitNet(60000);           /* 重启后要重新注网, 跟上电时一样 */
}

/**
  * @brief  把睡掉的时间补进毫秒时基。
  *
  *  TIM4 在 STOP 期间**停走**(APB1 时钟被停了), 所以睡完一觉 BC28_Millis()
  *  会少掉整整一段。而这个数被上报成 UP= 给服务端当"板子开机多久"用,
  *  不补的话服务端会看到时间倒流。
  *
  *  先调一次 BC28_Millis() 把睡前那几格 CNT 结算掉, 再加, 免得把睡觉前的
  *  零头算两遍。
  */
void BC28_MillisAdd(uint32_t ms)
{
    (void)BC28_Millis();
    ms_total += ms;
}

/**
  * @brief  丢掉 STOP 期间卡在 USART2 里的东西。
  *
  *  两种残留, 两种处理:
  *    硬件收了一半的字节 —— 进 STOP 那一刻 USART 可能正收着, 时钟一停就断在
  *      中间, 那个字节永远不会完整。必须把 DR 读空: 不读的话 RXNE 一直挂着,
  *      后面 wait_for 会把这半个字节当成模块的回应。
  *    软件缓冲里睡前积的字节 —— 用 rx_drop_all() 清。**但它是先 urc_scan()
  *      再清的**: 这几百毫秒里要是正好来了 +NSOCLI(socket 被运营商掐了),
  *      直接扔掉就等于把这个事件吃了。"丢数据"和"丢事件"是两回事。
  */
void BC28_RxFlush(void)
{
    uint32_t guard;

    for (guard = 0; guard < 32UL; guard++)
    {
        if (USART_GetFlagStatus(USART2, USART_FLAG_RXNE) == RESET)
        {
            break;
        }
        (void)USART2->DR;
    }

    /* 读一次 SR(上面那句) 再读 DR 才清得掉。 */
    if (USART_GetFlagStatus(USART2, USART_FLAG_ORE) != RESET)
    {
        (void)USART2->DR;
    }
    if (USART_GetFlagStatus(USART2, USART_FLAG_FE) != RESET)
    {
        (void)USART2->DR;
    }

    rx_drop_all();
}

/**
  * @brief  睡 ms 毫秒 —— **整个省电功能唯一的入口**。
  *         返回实际睡掉的毫秒数(已经补进 BC28_Millis)。
  *
  *  RTC 唤醒源不可用(rtc_ok=0)或者时间太短, 就**退回 BC28_Idle 空转**:
  *  照样等够这么久, 只是不省电。**绝不进 STOP。**
  *
  *  醒来那六步的**顺序是死的**, 每一步都对应一个具体的死法:
  *    1) 关唤醒源          —— 不关的话下一次 sleep_arm 装不上新值
  *    2) clk_to_32m()      —— 不重建: UART/时基/微秒延时一起错
  *    3) SysTick_Config()  —— 不接回来: Delay() 里 `while (TimingDelay != 0);`
  *                            永远等下去。**这是最直接的变砖路径**
  *    4) BC28_MillisAdd()  —— 不补: UP= 时间倒流
  *    5) BC28_RxFlush()    —— 不清: 半截字节被当成模块的回应
  *    6) wake_check()      —— 不探: 模块要是也没醒, 要等到下次失败计数满
  */
int BC28_SleepMs(uint32_t ms)
{
    uint32_t secs;
    uint32_t slept;

    if (ms == 0UL)
    {
        return 0;
    }

    if ((rtc_ok == 0) || (ms < BC28_SLEEP_MIN_MS))
    {
        BC28_Idle(ms);                 /* 逃生路线: 空转, 但照样等够 */
        return (int)ms;
    }

    /* 唤醒定时器是 1 秒一格的, 不足 1 秒进 1 秒。多睡的那零点几秒不影响 ——
       调用方(主循环)是按 BC28_Millis 判退出的, 补回去的也是整秒。

       ⚠ 先除再收, **不能写 (ms + 999) / 1000**: ms 接近 UINT32_MAX 时那个
         加法会先溢出成一个很小的数 —— "要睡 49 天"变成"睡 0 秒", 而且不报错。
         上限也在这里夹死, 保证 slept(= secs x 1000)和真正装进 WUTR 的
         是**同一个数**; 不然补进时基的量会跟实际睡的对不上。 */
    secs = (ms / 1000UL) + (((ms % 1000UL) != 0UL) ? 1UL : 0UL);

    /* 上限**跟着标定系数走**, 不写死 65536: sleep_arm 要把 secs 折成"数",
       折完之后不能超过 WUTR 的 16 位。不在这儿夹的话, 长睡会被 sleep_arm
       静默截到 65536 个数(≈15 小时), 而 slept 还按原 secs 补进时基 ——
       补多的部分就是纯虚数。 */
    {
        uint32_t max_secs = (65536UL * 1024UL) / wut_tps_q10;

        if (secs > max_secs) { secs = max_secs; }
    }
    if (secs < 1UL) { secs = 1UL; }

    if (sleep_arm(secs) != 0)
    {
        printf("! sleep_arm failed -- busy-wait %lu ms instead\r\n",
               (unsigned long)ms);
        BC28_Idle(ms);
        return (int)ms;
    }

    /* ---- 睡前: 关掉一切会把 WFI 立刻打醒的东西 ---- */
    SysTick->CTRL = 0;                          /* ★ 不关就进不去 STOP */
    SysTick->VAL  = 0;
    SCB->ICSR     = (uint32_t)SCB_ICSR_PENDSTCLR_Msk;

    /* NVIC 里只要还剩**一个**挂起位, WFI 就会立刻返回。这里把本工程真正
       使能过的那几条清掉(USART1 是调试口, 它的 RXNE 中断是开着的)。 */
    NVIC_ClearPendingIRQ(USART1_IRQn);
    NVIC_ClearPendingIRQ(USART2_IRQn);
    NVIC_ClearPendingIRQ(RTC_WKUP_IRQn);

    sleep_cnt++;

    /* ---- 进 STOP。这一句返回就是被 RTC 叫醒了 ---- */
    PWR_EnterSTOPMode(PWR_Regulator_LowPower, PWR_STOPEntry_WFI);

    /* ================= 醒了。下面六步顺序不能动 ================= */
    (void)RTC_WakeUpCmd(DISABLE);              /* 1 */
    clk_to_32m();                              /* 2 */

    if (SysTick_Config(SystemCoreClock / 1000) != 0)   /* 3 */
    {
        /* 唯一会失败的原因是 reload 超范围(32MHz/1000 不可能)。真碰上了
           就复位 —— 让 Delay() 里的 while 死等是更糟的结局。 */
        NVIC_SystemReset();
    }

    /* 处理函数里已经清过一次, 这里再清一次是幂等的兜底: 醒来时 WUT 还在
       继续跑, 万一在我们回来之前又溢出了一次, 挂起位就还挂着。 */
    BC28_WutClear();

    slept = secs * 1000UL;
    BC28_MillisAdd(slept);                     /* 4 */
    BC28_RxFlush();                            /* 5 */
    wake_check();                              /* 6 */

    sleep_ms_sum += slept;
    return (int)slept;
}

/**
  * @brief  RTC 唤醒源状态: 0 = 不可用(**不会进 STOP**, 退回空转) / 1 = LSE / 2 = LSI。
  *         用 LSI 时时间基准本身就是不准的, UP= 会跟着漂 —— 开机务必打印出来。
  */
uint8_t BC28_RtcOk(void)
{
    return rtc_ok;
}

uint32_t BC28_SleepCnt(void)
{
    return sleep_cnt;
}

uint32_t BC28_SleepMsSum(void)
{
    return sleep_ms_sum;
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
    case BC28_E_NORESP:  return "no-response";
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
  * @brief  把模块回的一整段(里面是 \r\n 分隔的多行)压成一行打出来。
  *
  *  为什么要它: 诊断"注不上网"最需要看的就是模块的**原始回话**, 但那段里全是
  *  CR/LF, 直接打出去会把串口日志切得七零八落, 分不清哪行是谁打的。这里把
  *  CR/LF 压成一个 '|'、丢掉不可打印字符。
  *
  *  ⚠ 栈: tmp[80]。这是省电功能加完之后本工程唯一新开的局部数组。改这个函数
  *    之后**必须复核 Maximum Stack Usage**(预算只剩 320 字节)。
  */
static void diag_line(const char *tag, const char *s)
{
    char     tmp[80];
    uint32_t i = 0;
    uint32_t n = 0;

    while ((s[n] != '\0') && (i < (sizeof(tmp) - 1UL)))
    {
        char c = s[n];

        n++;
        if ((c == '\r') || (c == '\n'))
        {
            if ((i > 0UL) && (tmp[i - 1UL] != '|'))
            {
                tmp[i] = '|';
                i++;
            }
        }
        else if ((c >= 0x20) && (c < 0x7F))
        {
            tmp[i] = c;
            i++;
        }
    }
    tmp[i] = '\0';
    printf("  %s \"%s\"\r\n", tag, tmp);
}

/**
  * @brief  轮询 AT+CEREG? 直到注网。0 = 已注网。
  *         1 = 已注册(本地), 5 = 已注册(漫游, NB-IoT 常见)。
  *         用轮询而不是等 URC, 是因为 URC 要提前 AT+CEREG=1 打开, 而这块板
  *         每次上电都要重新配 —— 轮询更笨但更稳。
  *
  *  ★ 2026-09-16 加的诊断(每 10 秒一条): 起因是板子在省电档跑了 17 轮之后
  *    静默了 22 小时, 而串口只有一句 `network attach timeout: timeout` ——
  *    **这一句话把最关键的信息全丢了**。注不上网只有三种可能, 处理方式完全不同:
  *      rc != 0        -> 模块连 AT 都不回。不是网络问题, 是模块掉电/死透,
  *                        换卡换天线都没用, 只有硬断电。
  *      stat == 2      -> 在搜网。天线/信号/位置。
  *      stat == 3      -> 被网络**拒绝**。几乎总是卡的问题(欠费/未激活/机卡绑定)。
  *      stat == 0      -> 没在搜也没注册, 通常是模块还没起来。
  *    没有这一条, 上面四种情况在现场看起来**一模一样**。
  */
int BC28_WaitNet(uint32_t to_ms)
{
    uint32_t t0     = BC28_Millis();
    uint32_t t_diag = 0;
    uint32_t n_diag = 0;
    int      rc;
    int      heard  = 0;        /* 模块至少回过一次 AT+CEREG? —— 见下面的返回值说明 */

    for (;;)
    {
        int stat = -1;

        rc = at_cmd("AT+CEREG?", "OK", BC28_TO_LONG, line, sizeof(line));
        if (rc == 0)
        {
            const char *p;
            const char *q;

            heard = 1;              /* 模块吭声了, 这一条跟"注没注上网"是两件事 */
            p = strstr(line, "+CEREG:");

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

        /* 第一次立刻打, 之后每 10 秒一条。line 这时已经解析完了, 可以复用。 */
        if ((n_diag == 0UL) || ((uint32_t)(BC28_Millis() - t_diag) >= 10000UL))
        {
            t_diag = BC28_Millis();
            n_diag++;

            if (rc != 0)
            {
                printf("  net[%lu] AT+CEREG? no answer (%s)\r\n",
                       (unsigned long)n_diag, BC28_Why(rc));
            }
            else
            {
                printf("  net[%lu] CEREG stat=%d\r\n",
                       (unsigned long)n_diag, stat);
                diag_line("cereg", line);
            }

            if (at_cmd("AT+CSQ", "OK", BC28_TO_LONG, line, sizeof(line)) == 0)
            {
                diag_line("csq", line);
            }
            else
            {
                printf("  net[%lu] AT+CSQ no answer\r\n", (unsigned long)n_diag);
            }
        }

        if ((uint32_t)(BC28_Millis() - t0) >= to_ms)
        {
            /* ★ 两种"超时"对调用方意义完全相反, 必须分开报:
                 有回话(BC28_E_TIMEOUT) = 模块活着, 只是在搜小区 → **该接着等**。
                   这一档千万别去 AT+NRB —— NRB 会让它从头搜一遍, 而搜一遍要 2 分钟,
                   正好比 main.c 那圈重试还长, 于是永远搜不完(2026-09-16 实测卡死)。
                 没回话(BC28_E_NORESP) = 模块一个字都没回 → 这时 NRB 才是对的。 */
            return (heard != 0) ? BC28_E_TIMEOUT : BC28_E_NORESP;
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

    /* 这两个默认 -1 = "没读到"。**不能留 memset 给的 0** —— 0 是合法的小区号,
       分不清"真读到 0"和"根本没读到"就没法在报文里可靠地省略这两个字段。
       见 bc28.h 里 BC28_RADIO 那段注释。 */
    r->cell_id = -1;
    r->pci     = -1;

    rc = at_cmd("AT+NUESTATS", "OK", BC28_TO_STATS, line, sizeof(line));
    if (rc < 0)
    {
        return rc;
    }

    (void)nstats_val(line, "Signal power", &r->rsrp);
    (void)nstats_val(line, "SNR",          &r->snr);
    (void)nstats_val(line, "ECL",          &r->ecl);
    (void)nstats_val(line, "CURRENT BAND", &r->band);

    /* 当前小区 —— 就在上面这条 NUESTATS 里, **不额外发 AT 命令**(多一条命令
       就多一次超时机会, 而且这块板子的射频本来就不稳)。 */
    (void)nstats_val(line, "Cell ID", &r->cell_id);
    (void)nstats_val(line, "PCI",     &r->pci);

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
        /* 回的是 OK 却没有号(或者干脆回 ERROR) —— 这正是 socket 漏光时的样子。
           这一笔不能记账: 号根本没拿到, 记上去反而把泄漏数算多了。 */
        return BC28_E_NOSOCK;
    }

    sock_open_n++;              /* 真拿到号了才算开了一个 (见 sock_open_n 那段) */
    return rc;
}

/**
  * @brief  DNS 解析失败时, 把"到底是哪一段断了"打出来。
  *
  *  为什么需要它 —— 2026-09-16 板子离线 22 小时, 现场是这样的:
  *    - CEREG 说已注网(stat=1/5), CSQ=26 / RSRP=-71.9, 信号好得很
  *    - AT+QDNS=0,<域名> 回 OK(受理了), 但结果那条 "+QDNS:" 30 秒不来
  *      (DumpRx 打出来只有一个空的 "\n\n"), 再发一次直接 ERROR
  *    - 同一个域名在本机 / 8.8.8.8 / VPS 上三处都能解析, DDNS 记录是活的
  *  也就是说 **DNS 这个动作失败了, 但完全看不出是"域名解析"这一层的问题,
  *  还是根本没有数据面**。NB-IoT 上"附着(CEREG)"和"有 IP(PDP 激活)"是
  *  两件事 —— 注网成功不等于能收发字节, 而 AT+QDNS 的报错把这两件事
  *  混成了一个"解析不了"。下面三个 AT 就是用来把这两层分开的:
  *
  *    AT+CGPADDR=0   模块到底分到 IP 没有(空 = 没有)
  *    AT+CGACT?      PDP 上下文激活了没有
  *    AT+CGDCONT?    APN 配的什么(注网能过但 APN 不对, 一样解析不了)
  *
  *  ⚠ 不新开局部数组 —— 复用模块层的静态 line[](栈本来只剩 320 字节)。
  */
static void dns_diag(const char *host)
{
    printf("  dns: probing why \"%s\" would not resolve\r\n", host);

    if (at_cmd("AT+CGPADDR=0", "OK", BC28_TO_LONG, line, sizeof(line)) == 0)
    {
        diag_line("pdp-ip ", line);
    }
    else
    {
        printf("  pdp-ip  no answer to AT+CGPADDR=0\r\n");
    }

    if (at_cmd("AT+CGACT?", "OK", BC28_TO_LONG, line, sizeof(line)) == 0)
    {
        diag_line("pdp-act", line);
    }
    else
    {
        printf("  pdp-act no answer to AT+CGACT?\r\n");
    }

    if (at_cmd("AT+CGDCONT?", "OK", BC28_TO_LONG, line, sizeof(line)) == 0)
    {
        diag_line("pdp-apn", line);
    }
    else
    {
        printf("  pdp-apn no answer to AT+CGDCONT?\r\n");
    }
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
        dns_diag(host);
        return rc;
    }

    /* ② "+QDNS:" = 结果这一条到了。**DNS 解析本身要花时间, 多则 2~3 秒**,
       所以这里的超时必须给足 —— 用 URC 那档 30 秒, 不能用 LONG。
       (第一版把解析结果也当成"跟 OK 同一条", 等到的只有一个光秃秃的 OK。) */
    rc = wait_for("+QDNS:", BC28_TO_URC, NULL, 0);
    if (rc < 0)
    {
        BC28_DumpRx();
        dns_diag(host);
        return rc;
    }

    /* ③ 冒号后面那半截就是 IP —— 接着读到换行为止 */
    n = read_field(out, cap, BC28_TO_LONG);
    if (n < 0)
    {
        BC28_DumpRx();
        dns_diag(host);
        return n;
    }

    /* 有些固件解析不出来时给的是 0.0.0.0。这不是"解析成功",
       当成失败报出去 —— 否则会拿 0.0.0.0 去 NSOCO, 白等 30 秒才超时。 */
    if (strcmp(out, "0.0.0.0") == 0)
    {
        dns_diag(host);
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

/**
  * @brief  关 socket。已经关掉的 socket 再关一次模块回 ERROR, 无害。
  *
  *  ★ 这里是 socket 泄漏的根因, 2026-09-17 晚实测出来的 (详见 bc28.h 里
  *     BC28_TO_CLOSE 那段)。原来只用 BC28_TO_LONG = **2 秒**等一句 OK,
  *    而一个刚建连失败(NSOCO 超时 30 秒)的 socket, 模块正忙着在 TCP 层
  *    重传, 2 秒内根本回不上话 —— 于是"关不掉", 号就留在模块里了。
  *
  *    现在: 每次等 BC28_TO_CLOSE(10 秒), 失败**隔 1 秒再试**, 一共 3 次。
  *    中间那 1 秒不是白等的 —— BC28_Idle() 会收字节并扫 URC, 正好让模块
  *    把 TCP 那边的烂摊子收一收, 也顺手接住可能来的 +NSOCLI。
  *
  *    ⚠ 只认 OK。回 ERROR 或者超时都**不算关掉**, 而且都要记账 —— 这个返回值
  *      不只是给调用方看的, 它还是上面那个账本的唯一凭据。 */
int BC28_Close(int sock)
{
    char cmd[24];
    int  rc = BC28_E_ARG;
    int  try_n;

    if (sock < 0)
    {
        return BC28_E_ARG;
    }
    sprintf(cmd, "AT+NSOCL=%d", sock);

    for (try_n = 0; try_n < BC28_CLOSE_TRY; try_n++)
    {
        rc = at_cmd(cmd, "OK", BC28_TO_CLOSE, NULL, 0);
        if (rc == 0)
        {
            sock_close_n++;
            return 0;
        }
        if (try_n < (BC28_CLOSE_TRY - 1))
        {
            BC28_Idle(1000);
        }
    }

    /* 三次都没关掉 = 真漏了一个。打出来 —— 这一行是"泄漏正在发生"最直接的证据,
       而且它会**在漏光之前**出现(配合账本那行, 能看出是从哪一轮开始斜的)。
       代价: 下次开 socket 之前我们手里少了一个号, 6 个里少一个。 */
    printf("! NSOCL=%d failed x%d (%s) -- socket NOT returned\r\n",
           sock, BC28_CLOSE_TRY, BC28_Why(rc));
    return rc;
}

/** @brief  账本读数(见 sock_open_n 上面那段)。差 > 0 = 有 socket 没关掉。 */
uint32_t BC28_SockOpenCnt(void)   { return sock_open_n;  }
uint32_t BC28_SockClosedCnt(void) { return sock_close_n; }

int BC28_SockBalance(void)
{
    return (int)(sock_open_n - sock_close_n);
}

/** @brief  最近一次 +NSOCLI 报的 socket 号, -1 = 从来没收到过。 */
int BC28_SockCliLast(void) { return sock_cli_last; }

/** @brief  收到过几次 +NSOCLI(累计, 跨 NRB 不清 —— 它记的是"网络关过几回")。 */
uint32_t BC28_SockCliCnt(void) { return sock_cli_cnt; }
