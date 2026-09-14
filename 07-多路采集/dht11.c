/**
  ******************************************************************************
  * @file    HARDWARE/dht11/dht11.c
  * @brief   DHT11 温湿度驱动 —— 自己写的, 不搬厂家 HARDWARE/DHT11。
  *          配套手册: 第 5 篇 · 多路采集与云端看板
  *
  *  这个文件只做两件事:
  *
  *    1) 一个 **1 微秒一格** 的自由跑计数器(TIM6, 不开中断, 不碰
  *       stm32l1xx_it.c)。
  *    2) 按 DHT11 的单总线时序把 40 位读回来。但注意 —— **不是**"延时之后
  *       去采样", 而是"把每个边沿的时刻记下来, 宽度算出来"。
  *
  *  ---------------------------------------------------------------------------
  *  为什么非要这么做(本驱动唯一有技术难度的地方)
  *
  *    DHT11 的 0 和 1 **只差在高电平的宽度**: 0 是 26~28us, 1 是 70us。
  *    门槛取 50us, 两边余量各约 20us —— 这套时序是实打实要微秒级精度的。
  *
  *    而本工程里没有任何现成的微秒延时可用:
  *      - TIM4 被 BC28_Millis() 占了 (1kHz 毫秒时基)
  *      - SysTick 被 main.c 的 1ms 中断占了 (Delay() 靠它)
  *      - 厂家那份 delay.c 会重写 SysTick->LOAD/VAL/CTRL, **直接破坏应用
  *        自己的中断时基** —— 连碰都不能碰
  *
  *    三条路, 只有第三条能走:
  *
  *      [x] 用 __NOP() 标定一个延时循环
  *          -O0 下循环体是 LDR/ADDS/STR/CMP/LDR/BLT, 耗时是**这一版编译的
  *          产物**而不是 CPU 的性质, 换个优化等级就废。更要命的是它
  *          **只能延时、不能测量** —— 等于把 20us 的余量交给全工程最不可控
  *          的那一项。
  *
  *      [x] 用 DWT 的 CYCCNT 周期计数器
  *          技术上跑得起来, 但 CYCCNT 在**调试电源域**里: 第 5 篇后面要加
  *          低功耗, 一进 sleep 它就停, 等待循环会变成死循环。而且它跟调试器
  *          共用 DWT, 又**没有第二个时钟能自检** —— 下一条才是关键。
  *
  *      [v] TIM6 自由跑 + 记边沿时刻(本文件的做法)
  *          精度来自 32MHz 晶振, 不是编译器 -> 换优化等级不影响;
  *          最坏的中断延迟(一个 ~5us 的 USART1 中断)只会把**某一个**边沿
  *          推后 <=5us, 而余量有 20us;
  *          而且它**自己就能被自检** —— 20ms 起始脉冲就是拿它计时的,
  *          打出来的 start_us 就是"尺子准不准"的读数。这跟 main.c 里那个
  *          "Delay(1000) 到底是不是 1000ms"的自检是同一个套路, 也正是
  *          第 3 篇栽过的那类跟头(时基快了 1000 倍, 现象却像模块死了)。
  *
  *  ---------------------------------------------------------------------------
  *  时序(手册第 7 页那张图)
  *
  *    主机: 拉低 >=18ms, 然后释放
  *    从机: 应答 80us 低 + 80us 高, 然后 40 位
  *    每一位: 50us 低, 然后 26~28us 高 = 0 / 70us 高 = 1
  *    第 5 个字节 = 前 4 个字节相加取低 8 位
  *
  *  ---------------------------------------------------------------------------
  *  ⚠ **字符串字面量一律 ASCII。** AC5 在字符串字面量里遇到中文会报
  *    warning #870-D(注释里写中文没问题, 注释在编译前就被剥掉了),
  *    而本工程要求 0 Error / 0 Warning。
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "dht11.h"
#include <string.h>

/* Private define ------------------------------------------------------------*/

/* ---- 超时(单位: 微秒) --------------------------------------------------
   每个数都是"手册标称 x 余量", 余量倍数写在后面。 */

/* 起始信号拉低时长。手册要求 >=18ms, 取 20ms。
   ⚠ 这个数同时是**尺子的自检基准** —— 打出来的 start_us 应当是 20000 上下,
     差一个数量级就说明 TIM6 分频配错了, 后面所有数字都不必信。 */
#define DHT11_T_START_US    20000

/* 响应信号: 低 80us + 高 80us。给 100us 的等待上限(约 1.25 倍)。 */
#define DHT11_TO_RESP_US    100

/* 等一个边沿的上限。要盖住"位与位之间那段 ~50us 低电平" + 余量, 取 150us。
   一位最长是 50 + 70 = 120us, 所以 150us 也够等完整的一位。 */
#define DHT11_TO_HI_US      150

/* 判 0 / 1 的门槛。0 的高电平 26~28us, 1 的是 70us, 50us 正好在中间,
   两边各留约 20us —— 这就是整套时序的全部余量, 也是 hi0_max / hi1_min
   两个实测值要盯的东西。 */
#define DHT11_T_HI_US       50

/* Private macro -------------------------------------------------------------*/

/* 拉低: 开漏输出。
   ⚠ 直接用寄存器而不是 GPIO_ResetBits() —— 后者是函数不是宏, -O0 下每次
     调用要 1~2us, 在一个 80us 的响应窗口里就是 2% 的误差, 而且它是**变动**
     延迟(取决于要不要走 PLT), 属于最不该引入的那类误差。
   ⚠ STM32L1 这份 CMSIS 头文件里 GPIO 结构体**没有 32 位的 BSRR**,
     而是拆成了两个 16 位寄存器(stm32l1xx.h):
         BSRRL -> 低半, 置位(输出高)
         BSRRH -> 高半, 复位(输出低)
     照 F4 的习惯写 GPIOB->BSRR 会直接编译不过(#136 没有这个成员)。
     两个拼起来才是那一个 32 位的 BSRR。 */
#define DHT11_LOW()         (DHT11_PORT->BSRRH = (uint16_t)DHT11_PIN)

/* 释放: 开漏 + 上拉 -> 输出高就是"不驱动", 线由模块那颗 4.7k 拉高。
   全程**不切方向**, 所以运行期一次 GPIO_Init 都不用调(那个要 3~10us)。 */
#define DHT11_RELEASE()     (DHT11_PORT->BSRRL = (uint16_t)DHT11_PIN)

/* Private function prototypes -----------------------------------------------*/
static void tim6_init(void);
static int  wait_level(int want, uint16_t to_us, uint16_t *t);

/* =========================================================================
 * 一、微秒时基: TIM6 自由跑, 1 格 = 1 微秒
 * ========================================================================= */

/**
  * @brief  把 TIM6 配成 **1MHz** 自由计数器。不开中断, 不碰 stm32l1xx_it.c。
  *
  *         STM32L151 上 TIM6 是基本定时器(只有 CNT/PSC/ARR, 没有通道),
  *         本工程里没人用: 串口是 USART1/2/3, 墨水屏是软件 SPI,
  *         TIM4 归 BC28_Millis(), SysTick 归 Delay()。所以拿它当尺子正好。
  *
  *  ⚠ 预分频这里除的是 1000000 —— 跟 bc28.c 里 tim4_init() 那个 1000
  *     只差三个零, 但差 1000 倍。第 3 篇就是在这儿栽的(那把尺子快了 1000 倍,
  *     现象却是"AT 打了 9000 次一个字节没回来", 看着像模块死了)。
  *     这一篇的自检点在 DHT11_Read() 里: start_us 应当是 20000 上下。
  */
static void tim6_init(void)
{
    TIM_TimeBaseInitTypeDef tb;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM6, ENABLE);

    TIM_TimeBaseStructInit(&tb);
    tb.TIM_Prescaler     = (uint16_t)(SystemCoreClock / 1000000) - 1;  /* 1MHz: 每格 1us */
    tb.TIM_Period        = 0xFFFF;      /* 65536 格回绕一次 = 65.5 毫秒 */
    tb.TIM_CounterMode   = TIM_CounterMode_Up;
    tb.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInit(TIM6, &tb);
    TIM_Cmd(TIM6, ENABLE);
}

/**
  * @brief  等总线变成 want 电平(1 = 高, 0 = 低), 最多等 to_us 微秒。
  *         等到了把**那一刻** TIM6 的读数写进 *t(调用方拿去做 16 位无符号
  *         相减, 回绕天然处理), 返回 0; 超时返回 DHT11_E_TIMEOUT。
  *
  *         ⚠ 这里是**轮询 GPIOB->IDR**, 不是调 GPIO_ReadInputDataBit() ——
  *           那个是函数, -O0 下每次 1~2us, 一帧要读 80 多次, 是整个设计里
  *           最大的一项误差来源。
  *
  *         ⚠ 也**没有关中断**。USART1 那个 ~5us 的中断顶多把某一个边沿的
  *           时刻推后 5us, 而门槛两边各留 20us —— 关中断换来的那点确定性
  *           不值得, 何况关 25ms 会把 BC28 的 URC 全堵在门外。
  */
static int wait_level(int want, uint16_t to_us, uint16_t *t)
{
    uint16_t t0 = (uint16_t)TIM6->CNT;
    uint16_t now;

    for (;;)
    {
        if ((((GPIOB->IDR & DHT11_PIN) != 0) ? 1 : 0) == want)
        {
            *t = (uint16_t)TIM6->CNT;
            return 0;
        }

        now = (uint16_t)TIM6->CNT;
        if ((uint16_t)(now - t0) > to_us)
        {
            return DHT11_E_TIMEOUT;
        }
    }
}

/* =========================================================================
 * 二、初始化
 * ========================================================================= */

/**
  * @brief  PB8 配成**开漏输出 + 上拉**, 并拉起微秒时基。上电调一次。
  *
  *  为什么是开漏而不是推挽(这是这个驱动最要紧的一处硬件决定):
  *
  *    1) **运行期不用切方向。** 单总线要"一会儿输出一会儿输入", 常规写法是
  *       每次换向都调一次 GPIO_Init(3~10us), 一帧 80 多次 —— 根本来不及。
  *       开漏下"输出高"就等于"松手", 读 IDR 任何时候都能读线上真实电平
  *       (STM32 的输入施密特触发器除模拟模式外一直是挂着的)。
  *
  *    2) **不可能跟传感器打架。** 配成推挽又驱动高电平的时候, 传感器同时
  *       往下拉, 就是一条从 VDD 到 GND 的直通短路 —— 开漏下主机只会"松手",
  *       最坏也就是读到一个低电平。
  *
  *  ⚠ **GPIOB 的时钟必须在这里自己开。** 全工程唯一一处
  *    RCC_AHBPeriph_GPIOB 在 usart.c 的 uart3_init() 里, 而那个函数
  *    **从来没被调用过**。不开时钟的话, 对 PB8 的所有写都是**静默失效**的
  *    (不报错, 就是没反应), 现象跟"传感器坏了/没插"一模一样, 极难查。
  *
  *  ⚠ 板子这侧**没有上拉电阻**(原理图 PB8 那个网络上没有), 所以总线空闲时
  *    的高电平全靠**模块自带的那颗 4.7k**。这里配的 GPIO_PuPd_UP 是 STM32
  *    内部的弱上拉(约 40k), 只能算个保底; 也正因为有它, 模块没插的时候
  *    线不会悬空乱飘, 现象反而是干净的 E_NORESP。
  */
void DHT11_Init(void)
{
    GPIO_InitTypeDef gi;

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_GPIOB, ENABLE);   /* 见上面那段警告 */
    tim6_init();

    GPIO_StructInit(&gi);
    gi.GPIO_Pin   = DHT11_PIN;
    gi.GPIO_Mode  = GPIO_Mode_OUT;
    gi.GPIO_OType = GPIO_OType_OD;      /* 开漏是关键 */
    gi.GPIO_PuPd  = GPIO_PuPd_UP;
    gi.GPIO_Speed = GPIO_Speed_400KHz;  /* 单总线最慢的边沿也够用了 */
    GPIO_Init(DHT11_PORT, &gi);

    DHT11_RELEASE();                    /* 空闲: 松手, 让上拉把线拉高 */
}

/* =========================================================================
 * 三、一次完整读数
 * ========================================================================= */

/**
  * @brief  读一次温湿度。阻塞约 25ms(20ms 起始 + 约 5ms 的 40 位)。
  *         返回 0 成功, < 0 是 dht11.h 里那些错误码。
  *
  *         即使返回错误, d 里的**实测量**也已经填好了(填到哪一步算哪一步) ——
  *         那是诊断接错线的唯一线索, 别浪费。
  *
  *         ⚠ 采样间隔别短于 DHT11_MIN_PERIOD_MS(2 秒), 见 dht11.h。
  */
int DHT11_Read(DHT11_DATA *d)
{
    uint16_t t_start;
    uint16_t t_low;
    uint16_t t_resp_up;
    uint16_t t_resp_dn;
    uint16_t t_rise;
    uint16_t t_fall;
    uint16_t prev_fall;
    uint16_t w;
    uint32_t sum;
    int      i;
    int      rc;
    int      bit;

    if (d == NULL)
    {
        return DHT11_E_ARG;
    }
    memset(d, 0, sizeof(*d));
    d->hi1_min = 0xFFFF;        /* 收尾时若还是这个值, 说明一个 1 都没读到 */

    /* ---- 1) 起始信号: 拉低 20ms ----
       这 20ms 同时也是**尺子的自检**: start_us 应当是 20000 上下。
       不是的话就是 TIM6 的分频配错了, 后面所有数字都不必信。 */
    t_start = (uint16_t)TIM6->CNT;
    DHT11_LOW();
    while ((uint16_t)((uint16_t)TIM6->CNT - t_start) < DHT11_T_START_US)
    {
        /* 空转。这里不调任何延时函数 —— 计的就是 TIM6 它自己。 */
    }
    d->start_us = (uint16_t)((uint16_t)TIM6->CNT - t_start);
    DHT11_RELEASE();

    /* ---- 2) 响应信号: 低 80us -> 高 80us -> 再拉低 ----
       **要查两次**, 不能只查"有没有变低": 只查一次的话, "线被短路到地"
       会被当成"传感器应答了"。E_NORESP 和 E_STUCK 分成两个码就是为这个。 */
    rc = wait_level(0, DHT11_TO_RESP_US, &t_low);
    if (rc < 0)
    {
        return DHT11_E_NORESP;      /* 一直没低下来: 没接 / 没上电 / 插错脚 */
    }

    rc = wait_level(1, DHT11_TO_RESP_US, &t_resp_up);
    if (rc < 0)
    {
        return DHT11_E_STUCK;       /* 低下去没起来: 短路到地 / 没供电 */
    }
    d->resp_low = (uint16_t)(t_resp_up - t_low);

    rc = wait_level(0, DHT11_TO_RESP_US, &t_resp_dn);
    if (rc < 0)
    {
        return DHT11_E_STUCK;       /* 没等到第二次拉低: 同上 */
    }
    d->resp_high = (uint16_t)(t_resp_dn - t_resp_up);

    /* 从这里开始量整帧。响应头 + 40 位最多 40*(50+70)+160 = 4960us,
       离 TIM6 的一个回绕周期(65.5ms)差得远, 所以一次 16 位相减就够,
       不需要 32 位累加器。 */
    prev_fall = t_resp_dn;

    /* ---- 3) 40 位: 每位 = 一段低 + 一段高, 高的长短就是这一位的值 ---- */
    for (i = 0; i < 40; i++)
    {
        rc = wait_level(1, DHT11_TO_HI_US, &t_rise);    /* 等这一位的上升沿 */
        if (rc < 0)
        {
            return DHT11_E_TIMEOUT;
        }
        rc = wait_level(0, DHT11_TO_HI_US, &t_fall);    /* 等它的下降沿 */
        if (rc < 0)
        {
            return DHT11_E_TIMEOUT;
        }

        /* 位与位之间那段低电平, 顺手量一下(应当 ~50us)。它不参与判 0/1,
           但它是"时序整体对不对"的旁证 —— 接错线的时候这个数会很离谱。 */
        w = (uint16_t)(t_rise - prev_fall);
        if (w > d->lo_max)
        {
            d->lo_max = w;
        }
        prev_fall = t_fall;

        w   = (uint16_t)(t_fall - t_rise);
        bit = (w > DHT11_T_HI_US) ? 1 : 0;

        /* 记余量: 判 0 的最宽高电平、判 1 的最窄高电平。
           这两个数分得越开, 说明门槛 50us 越安全。 */
        if (bit == 0)
        {
            if (w > d->hi0_max)
            {
                d->hi0_max = w;
            }
        }
        else
        {
            if (w < d->hi1_min)
            {
                d->hi1_min = w;
            }
        }

        d->raw[i >> 3] = (uint8_t)((d->raw[i >> 3] << 1) | (uint8_t)bit);
    }

    d->frame_us = (uint16_t)(prev_fall - t_resp_dn);
    if (d->hi1_min == 0xFFFF)
    {
        d->hi1_min = 0;         /* 全是 0, 不是"最小值还是初值" */
    }

    /* ---- 4) 校验和: 前 4 个字节相加, 取低 8 位 ---- */
    sum = (uint32_t)d->raw[0] + (uint32_t)d->raw[1]
        + (uint32_t)d->raw[2] + (uint32_t)d->raw[3];
    d->sum_ok = (((uint8_t)(sum & 0xFF)) == d->raw[4]) ? 1 : 0;
    if (d->sum_ok == 0)
    {
        return DHT11_E_SUM;
    }

    /* ---- 5) 合理性 ----
       校验和只有 8 位, 位错位之后**有 1/256 的概率照样撞对**。
       量程检查就是拦这种"看着合法其实全错"的。 */
    d->humi     = d->raw[0];
    d->humi_dec = d->raw[1];
    d->temp     = d->raw[2];
    d->temp_dec = d->raw[3];

    /* raw[2] 的最高位是负温符号位, 比较前先掩掉。DHT11 量程 0~50 摄氏度,
       上面那些字节校验对了还超 60, 那就是位错位。 */
    if ((d->humi > 100) || ((d->raw[2] & 0x7F) > 60))
    {
        return DHT11_E_RANGE;
    }

    return 0;
}

/* =========================================================================
 * 四、错误码 -> 人话
 * ========================================================================= */

/**
  * @brief  错误码翻成可以直接打串口的短串(**全 ASCII**, 理由见文件头)。
  *         每一条都带上"接下来该查什么" —— 手上只有一块板子的时候,
  *         串口里这一行就是全部线索。
  */
const char *DHT11_Why(int rc)
{
    switch (rc)
    {
    case 0:
        return "ok";
    case DHT11_E_ARG:
        return "bad argument (NULL)";
    case DHT11_E_NORESP:
        return "no response -- sensor not connected / not powered / wrong pin";
    case DHT11_E_STUCK:
        return "line stuck low -- shorted to GND, or no pull-up, or no power";
    case DHT11_E_TIMEOUT:
        return "timeout mid-frame -- bad wiring or unstable supply";
    case DHT11_E_SUM:
        return "checksum mismatch -- a bit was misread (retry usually fixes)";
    case DHT11_E_RANGE:
        return "value out of range -- bit slip that beat the checksum";
    default:
        return "unknown";
    }
}
