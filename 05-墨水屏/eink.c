/**
  ******************************************************************************
  * @file    HARDWARE/eink/eink.c
  * @brief   2.13" 单色墨水屏 (SSD1673) 最小驱动
  *
  *  时序全部照抄厂家驱动(本工程磁盘上那份 print-helloworld/HARDWARE/eink/smp.c,
  *  同一块板实测过), 只做三件事:
  *
  *   1) 砍掉 79KB Font.h / 图片数组 / 二维码 / 局部刷新窗口 —— 第一版只为"看效果";
  *   2) 修厂家两个 bug:
  *        a) RCC_APB2PeriphClockCmd(RCC_AHBPeriph_GPIOx, ENABLE)
  *           —— 函数和常量不配套: 这个函数写的是 APB2ENR, 传进去的却是
  *              AHBENR 的位号。后果: GPIOA 那行(0x01)歪打正着开了 SYSCFG,
  *              GPIOB 那行(0x02)落在一个保留位上、什么都没开 ——
  *              两行都没打开它们声称的 GPIO 时钟。
  *              厂家那份工程里看不出来, 是因为 GPIOA/B/C 的时钟早被串口
  *              初始化和 LED_Init 正确打开了(见 LED.c:5 / usart.c:61,165)。
  *              这里不赌初始化顺序, 自己一次性把 A/B/C 开齐。
  *        b) BUSY 被配成 GPIO_Mode_OUT, 然后 GPIO_ReadOutputDataBit() 读输出锁存器
  *           —— 复位后恒为 0, 所以 while(nBUSY==0) 一进去就 break, 是"假轮询",
  *              实际靠后面 DELAY_S(2) 硬等。这里配成输入 + 读真电平 + 带超时。
  *   3) 用整屏帧缓冲 + 一次全刷, 省掉厂家 EINK_ShowChar 里那两个 DELAY_S(5)
  *      —— 那才是厂家 demo"刷新要 20 秒"的真凶(10 秒/字符), 跟面板无关,
  *         面板本身一次全刷约 2 秒。
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "eink.h"
#include "font5x7.h"
#include "main.h"          /* Delay() —— 由 main.c 的 SysTick_Config 驱动 */
#include <string.h>

/* Private define ------------------------------------------------------------*/
/* 引脚: 跟厂家 smp.h 的定义一致 */
#define SDA_H    GPIO_SetBits(GPIOA, GPIO_Pin_7)
#define SDA_L    GPIO_ResetBits(GPIOA, GPIO_Pin_7)
#define SCLK_H   GPIO_SetBits(GPIOA, GPIO_Pin_5)
#define SCLK_L   GPIO_ResetBits(GPIOA, GPIO_Pin_5)
#define nCS_H    GPIO_SetBits(GPIOA, GPIO_Pin_4)
#define nCS_L    GPIO_ResetBits(GPIOA, GPIO_Pin_4)
#define nDC_H    GPIO_SetBits(GPIOB, GPIO_Pin_1)
#define nDC_L    GPIO_ResetBits(GPIOB, GPIO_Pin_1)
#define nRST_H   GPIO_SetBits(GPIOB, GPIO_Pin_0)
#define nRST_L   GPIO_ResetBits(GPIOB, GPIO_Pin_0)
/* BUSY 低有效: 低 = 空闲, 高 = 忙。这里读的是真输入电平(厂家读的是输出锁存器) */
#define nBUSY    GPIO_ReadInputDataBit(GPIOC, GPIO_Pin_5)

/* Private variables ---------------------------------------------------------*/
/* 帧缓冲: [栅极 0..249][源极字节 0..15]。1 = 白, 0 = 黑。
   4000 字节, STM32L151RC 有 32KB RAM, 放得下。 */
static unsigned char fb[EINK_W][16];

/* 全刷 LUT (0x32 寄存器), 29 字节, 逐字照抄厂家 init_data[] */
static const unsigned char lut_full_update[29] = {
    0x50, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x11,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
    0x01,
    0x00, 0x00, 0x00, 0x00, 0x00
};

/* 4x4 有序抖动阈值 —— 只有黑白两色时造灰阶 */
static const unsigned char dither4x4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5
};

/* 毫秒计数: SysTick 由 main.c 配成 1ms 周期, 读 CTRL 会清 COUNTFLAG,
   所以每个 ms 最多计一次。不占中断, 不动 stm32l1xx_it.c。 */
static volatile unsigned int g_ms;

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  给 SysTick 的 COUNTFLAG 记个数 —— 在忙等循环里反复调用即可得到 ms。
  */
static void TickPoll(void)
{
    if (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk)
    {
        g_ms++;
    }
}

/**
  * @brief  位间小延时。厂家用 DELAY_100nS(10)(空循环, 有可能被优化掉),
  *         这里用 __NOP 保证不被优化, 让软件 SPI 稳在 ~1MHz 量级。
  */
static void SpiBitDelay(void)
{
    unsigned char k;
    for (k = 0; k < 6; k++)
    {
        __NOP();
    }
}

/**
  * @brief  软件 SPI 发一个字节。dc=0 命令(DC 低), dc=1 数据(DC 高)。MSB 先出。
  */
static void EINK_SendByte(unsigned char dat, unsigned char dc)
{
    unsigned char i;

    TickPoll();                 /* 让全刷期间 g_ms 也在走 */

    nCS_L;
    SCLK_L;
    if (dc)
    {
        nDC_H;
    }
    else
    {
        nDC_L;
    }

    for (i = 0; i < 8; i++)
    {
        if (dat & 0x80)
        {
            SDA_H;
        }
        else
        {
            SDA_L;
        }
        SpiBitDelay();
        SCLK_H;
        SpiBitDelay();
        SCLK_L;
        dat = (unsigned char)(dat << 1);
    }

    nCS_H;
}

static void EINK_SendCmd(unsigned char cmd)
{
    EINK_SendByte(cmd, 0);
}

static void EINK_SendData(unsigned char dat)
{
    EINK_SendByte(dat, 1);
}

/**
  * @brief  写 0x32 波形 LUT。没有它就没有驱动波形, 屏不会显影 —— 不能跳。
  */
static void EINK_WriteLut(void)
{
    unsigned char i;
    EINK_SendCmd(0x32);
    for (i = 0; i < 29; i++)
    {
        EINK_SendData(lut_full_update[i]);
    }
}

/**
  * @brief  复位 + SSD1673 寄存器初始化。顺序不能改。
  */
static void EINK_InitPanel(void)
{
    unsigned char ok;

    /* 复位: RST 低 >=10ms, 再拉高 >=10ms */
    nRST_L;
    Delay(10);
    nRST_H;
    Delay(10);
    EINK_BusyWait(1000);

    EINK_SendCmd(0x01);             /* 栅极设置 */
    EINK_SendData(0xF9);            /* MUX = 250-1 = 249 */
    EINK_SendData(0x00);            /* GD=0 SM=0 TB=0: 从 G0 开始扫 */

    EINK_SendCmd(0x3A);             /* dummy line, 50Hz 帧频 */
    EINK_SendData(0x06);
    EINK_SendCmd(0x3B);             /* 栅极线宽, 50Hz */
    EINK_SendData(0x0B);            /* 78us*(250+6) = 19.968ms */

    EINK_SendCmd(0x3C);             /* 边框波形 */
    EINK_SendData(0x33);            /* GS1->GS1, 全刷时边框走白 */

    EINK_SendCmd(0x11);             /* 数据输入模式 */
    EINK_SendData(0x01);            /* Y 递减, X 递增, 地址按 X 方向更新 */

    EINK_SendCmd(0x44);             /* RAM X 起止: 0..15  -> 16 字节 = 128 源极 */
    EINK_SendData(0x00);
    EINK_SendData(0x0F);
    EINK_SendCmd(0x45);             /* RAM Y 起止: 249..0 */
    EINK_SendData(0xF9);
    EINK_SendData(0x00);

    EINK_SendCmd(0x2C);             /* VCOM */
    EINK_SendData(0x4B);            /* -1.4V */

    EINK_WriteLut();

    EINK_SendCmd(0x21);             /* 显示更新选项: 旁路(不按 LUT 走) */
    EINK_SendData(0x83);

    /* 先全白擦一遍: 防止上电残留/花屏 */
    EINK_Clear(EINK_WHITE);
    EINK_Refresh(&ok);

    EINK_SendCmd(0x21);             /* 恢复: 之后按 LUT 走 */
    EINK_SendData(0x03);
    EINK_SendCmd(0x3C);             /* 局部刷新时边框高阻 */
    EINK_SendData(0x73);
}

/* Exported functions --------------------------------------------------------*/

void EINK_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    /* 正确的 AHB 时钟使能 —— 厂家那两行是坏的, GPIOC 从来没开过 */
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_GPIOA | RCC_AHBPeriph_GPIOB |
                          RCC_AHBPeriph_GPIOC, ENABLE);

    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_7;  /* CS SCLK SDA */
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_0 | GPIO_Pin_1;               /* RST DC */
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_5;                            /* BUSY */
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IN;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    nCS_H;
    nDC_H;
    nRST_H;
    SCLK_L;

    EINK_InitPanel();
}

void EINK_Clear(unsigned char color)
{
    memset(fb, color ? 0xFF : 0x00, sizeof(fb));
}

void EINK_SetPixel(unsigned int x, unsigned int y, unsigned char color)
{
    unsigned char mask;

    if (x >= EINK_W || y >= EINK_H)
    {
        return;
    }
    mask = (unsigned char)(0x80 >> (y & 7));
    if (color)
    {
        fb[x][y >> 3] |= mask;
    }
    else
    {
        fb[x][y >> 3] &= (unsigned char)(~mask);
    }
}

void EINK_FillRect(unsigned int x0, unsigned int y0, unsigned int x1, unsigned int y1,
                   unsigned char color)
{
    unsigned int x, y;
    unsigned int b, b0, b1;

    if (x0 > x1) { x = x0; x0 = x1; x1 = x; }
    if (y0 > y1) { y = y0; y0 = y1; y1 = y; }
    if (x1 >= EINK_W) { x1 = EINK_W - 1; }
    if (y1 >= EINK_H) { y1 = EINK_H - 1; }
    if (x0 >= EINK_W || y0 >= EINK_H)
    {
        return;
    }

    b0 = y0 >> 3;
    b1 = y1 >> 3;

    for (x = x0; x <= x1; x++)
    {
        for (b = b0; b <= b1; b++)
        {
            /* 字节 b 覆盖 y = 8b..8b+7, bit7 对应 y=8b */
            unsigned char first = (b == b0) ? (unsigned char)(0xFF >> (y0 & 7)) : 0xFF;
            unsigned char last  = (b == b1) ? (unsigned char)(0xFF << (7 - (y1 & 7))) : 0xFF;
            unsigned char m     = (unsigned char)(first & last);

            if (color)
            {
                fb[x][b] |= m;
            }
            else
            {
                fb[x][b] &= (unsigned char)(~m);
            }
        }
    }
}

void EINK_Rect(unsigned int x0, unsigned int y0, unsigned int x1, unsigned int y1,
               unsigned char color)
{
    if (x0 > x1) { unsigned int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { unsigned int t = y0; y0 = y1; y1 = t; }

    EINK_FillRect(x0, y0, x1, y0, color);       /* 上 */
    EINK_FillRect(x0, y1, x1, y1, color);       /* 下 */
    EINK_FillRect(x0, y0, x0, y1, color);       /* 左 */
    EINK_FillRect(x1, y0, x1, y1, color);       /* 右 */
}

void EINK_Checker(unsigned int x0, unsigned int y0, unsigned int x1, unsigned int y1,
                  unsigned int cell, unsigned char color)
{
    unsigned int x, y;
    unsigned char inv = color ? EINK_BLACK : EINK_WHITE;

    if (cell == 0) { cell = 1; }
    if (x0 > x1) { x = x0; x0 = x1; x1 = x; }
    if (y0 > y1) { y = y0; y0 = y1; y1 = y; }

    for (x = x0; x <= x1 && x < EINK_W; x++)
    {
        for (y = y0; y <= y1 && y < EINK_H; y++)
        {
            EINK_SetPixel(x, y, (((x / cell) + (y / cell)) & 1) ? color : inv);
        }
    }
}

void EINK_VLines(unsigned int x0, unsigned int y0, unsigned int x1, unsigned int y1,
                 unsigned int spacing, unsigned char color)
{
    unsigned int x, y;

    if (spacing == 0) { spacing = 1; }
    if (x0 > x1) { x = x0; x0 = x1; x1 = x; }
    if (y0 > y1) { y = y0; y0 = y1; y1 = y; }

    for (x = x0; x <= x1 && x < EINK_W; x++)
    {
        if (((x - x0) % spacing) == 0)
        {
            for (y = y0; y <= y1 && y < EINK_H; y++)
            {
                EINK_SetPixel(x, y, color);
            }
        }
    }
}

void EINK_HLines(unsigned int x0, unsigned int y0, unsigned int x1, unsigned int y1,
                 unsigned int spacing, unsigned char color)
{
    unsigned int x, y;

    if (spacing == 0) { spacing = 1; }
    if (x0 > x1) { x = x0; x0 = x1; x1 = x; }
    if (y0 > y1) { y = y0; y0 = y1; y1 = y; }

    for (y = y0; y <= y1 && y < EINK_H; y++)
    {
        if (((y - y0) % spacing) == 0)
        {
            for (x = x0; x <= x1 && x < EINK_W; x++)
            {
                EINK_SetPixel(x, y, color);
            }
        }
    }
}

void EINK_Dither(unsigned int x0, unsigned int y0, unsigned int x1, unsigned int y1,
                 unsigned int level, unsigned char color)
{
    unsigned int x, y;
    unsigned char inv = color ? EINK_BLACK : EINK_WHITE;

    if (level > 16) { level = 16; }
    if (x0 > x1) { x = x0; x0 = x1; x1 = x; }
    if (y0 > y1) { y = y0; y0 = y1; y1 = y; }

    for (x = x0; x <= x1 && x < EINK_W; x++)
    {
        for (y = y0; y <= y1 && y < EINK_H; y++)
        {
            unsigned char t = dither4x4[((y & 3) << 2) | (x & 3)];
            EINK_SetPixel(x, y, (t < level) ? color : inv);
        }
    }
}

void EINK_Char(unsigned int x, unsigned int y, char ch, unsigned int scale)
{
    const unsigned char *g;
    unsigned int row, bit;

    if (scale == 0) { scale = 1; }
    if (ch < FONT5X7_FIRST || ch > FONT5X7_LAST) { ch = ' '; }

    g = &FONT5X7[((unsigned char)ch - FONT5X7_FIRST) * FONT5X7_H];

    for (row = 0; row < FONT5X7_H; row++)
    {
        for (bit = 0; bit < FONT5X7_W; bit++)
        {
            if (g[row] & (0x10 >> bit))
            {
                unsigned int px = x + bit * scale;
                unsigned int py = y + row * scale;
                EINK_FillRect(px, py, px + scale - 1, py + scale - 1, EINK_BLACK);
            }
        }
    }
}

void EINK_Text(unsigned int x, unsigned int y, const char *str, unsigned int scale)
{
    unsigned int step;

    if (scale == 0) { scale = 1; }
    step = (FONT5X7_W + 1) * scale;         /* 5 列 + 1 列字距 */

    while (*str)
    {
        if (x + FONT5X7_W * scale > EINK_W)
        {
            break;                          /* 不折行, 交给调用者排版 */
        }
        EINK_Char(x, y, *str, scale);
        x += step;
        str++;
    }
}

unsigned char EINK_BusyWait(unsigned int timeout_ms)
{
    unsigned int t0 = g_ms;

    while (nBUSY != 0)                      /* 高 = 忙 */
    {
        TickPoll();
        if ((g_ms - t0) > timeout_ms)
        {
            return 0;                       /* 超时: 屏没插好/没上电, 不硬等 */
        }
    }
    return 1;
}

unsigned int EINK_Refresh(unsigned char *ok)
{
    unsigned int t0;
    unsigned int x, b;
    unsigned char ok_local;

    t0 = g_ms;

    EINK_SendCmd(0x4E);                     /* RAM X 计数 = 0 */
    EINK_SendData(0x00);
    EINK_SendCmd(0x4F);                     /* RAM Y 计数 = 249 (第一列) */
    EINK_SendData(0xF9);

    EINK_SendCmd(0x24);                     /* 写 RAM */
    for (x = 0; x < EINK_W; x++)
    {
        for (b = 0; b < 16; b++)
        {
            EINK_SendData(fb[x][b]);
        }
    }

    EINK_SendCmd(0x22);                     /* 显示更新 */
    EINK_SendData(0xC7);                    /* 全刷 */
    EINK_SendCmd(0x20);                     /* 执行 */

    Delay(2);                               /* 让面板先把 BUSY 拉高 */
    ok_local = EINK_BusyWait(8000);

    if (ok)
    {
        *ok = ok_local;
    }

    return g_ms - t0;
}
