/**
  ******************************************************************************
  * @file    Project/Test/main.c
  * @brief   墨水屏最小驱动 · 综合测试页
  *
  *  一屏回答"显示效果是否如意":
  *    纯黑/纯白块   -> 对比度、均匀性、有没有竖条纹
  *    1px/2px 棋盘  -> 分辨率极限、边缘干净度
  *    1px 细线      -> 软件 SPI 时序稳不稳(断线 = 时序问题)
  *    多字号文字    -> 清晰度、笔画完整性
  *    抖动灰阶      -> 只有黑白两色时的过渡能力
  *    8px 大格棋盘  -> 残影(ghosting)最明显的图案
  *    最外框        -> 画在 RAM 物理边界上, 量真实可视区多大
  *
  *  printf() 由 HARDWARE/usart/usart.c 重定向到 USART1 (CH340)
  *  串口: 9600 8-N-1, 无流控
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usart.h"
#include "led.h"
#include "eink.h"

/* Private variables ---------------------------------------------------------*/
/* 由 SysTick_Handler (1ms) 递减 —— Delay() 靠它 */
static __IO uint32_t TimingDelay;

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  画综合测试页(只写帧缓冲, 不上屏)。
  */
static void DrawTestPage(void)
{
    EINK_Clear(EINK_WHITE);

    /* 最外框: 画在 RAM 的物理边界 0..127 上。
       如果最下面那条边看不见 -> 可视高度不到 128 (2.13" 标称 122)。 */
    EINK_Rect(0, 0, EINK_W - 1, EINK_H - 1, EINK_BLACK);

    /* 左上角实心方块 = 原点标记。看它落在哪个角, 就知道方向/镜像对不对 */
    EINK_FillRect(2, 2, 17, 17, EINK_BLACK);

    /* 标题 2 倍字 */
    EINK_Text(22, 2, "EINK 250X128", 2);

    /* 数字 2 倍字 */
    EINK_Text(4, 20, "0123456789", 2);

    /* 字母 1 倍字, 两行 */
    EINK_Text(4, 38, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 1);
    EINK_Text(4, 47, "0123456789 .-:/", 1);

    /* --- 图案带 y = 60..86 --- */
    EINK_FillRect(4, 60, 33, 86, EINK_BLACK);                 /* 纯黑块 30x27 */
    EINK_Rect(38, 60, 66, 86, EINK_BLACK);                    /* 白块 + 边框 */
    EINK_Checker(70, 60, 101, 86, 1, EINK_BLACK);             /* 1px 棋盘 */
    EINK_Checker(106, 60, 137, 86, 2, EINK_BLACK);            /* 2px 棋盘 */
    EINK_VLines(142, 60, 173, 86, 2, EINK_BLACK);             /* 竖细线, 间距 2 */
    EINK_HLines(178, 60, 209, 86, 2, EINK_BLACK);             /* 横细线, 间距 2 */
    EINK_Checker(214, 60, 245, 86, 8, EINK_BLACK);            /* 8px 大格 = 残影试纸 */

    /* --- 灰阶抖动带 y = 90..108 --- */
    EINK_Dither(4,    90, 61,  108, 4,  EINK_BLACK);          /* 25% */
    EINK_Dither(65,   90, 122, 108, 8,  EINK_BLACK);          /* 50% */
    EINK_Dither(126,  90, 183, 108, 12, EINK_BLACK);          /* 75% */
    EINK_FillRect(187, 90, 245, 108, EINK_BLACK);             /* 100% 实心 */

    /* --- 底部标签 --- */
    EINK_Text(4,   112, "25% 50% 75% 100%", 1);
    EINK_Text(150, 112, "GHOST/EDGE TEST", 1);
}

/**
  * @brief  Main program.
  */
int main(void)
{
    unsigned int ms;
    unsigned char ok;
    unsigned int i;

    /* 1 ms SysTick 中断 —— Delay() 和帧缓冲布局都用它 */
    if (SysTick_Config(SystemCoreClock / 1000))
    {
        while (1);
    }

    LED_Init();          /* PC3, 用来确认程序在跑 */
    uart1_init(9600);    /* USART1 -> CH340 -> USB1 */

    printf("\r\n\r\n");
    printf("########################################\r\n");
    printf("#  EINK minimal driver / test page     #\r\n");
    printf("#  SSD1673  250x128  bitbang SPI       #\r\n");
    printf("########################################\r\n");
    printf("build: " __DATE__ " " __TIME__ "\r\n\r\n");

    printf("[1] EINK_Init() ...\r\n");
    printf("    GPIO: SCLK=PA5 SDA=PA7 CS=PA4 DC=PB1 RST=PB0 BUSY=PC5\r\n");
    EINK_Init();
    printf("    done  (init ends with a full white refresh)\r\n\r\n");

    printf("[2] draw test page into 4000-byte framebuffer ...\r\n");
    DrawTestPage();
    printf("    done\r\n\r\n");

    printf("[3] full refresh #1 ...\r\n");
    ms = EINK_Refresh(&ok);
    printf("    %u ms   BUSY %s\r\n", ms, ok ? "released OK" : "TIMEOUT!");

    printf("[4] full refresh #2 (same content, ghosting check) ...\r\n");
    ms = EINK_Refresh(&ok);
    printf("    %u ms   BUSY %s\r\n\r\n", ms, ok ? "released OK" : "TIMEOUT!");

    printf("--------------------------------------------------\r\n");
    printf("Look at the panel:\r\n");
    printf("  - solid black square at the corner = origin marker\r\n");
    printf("  - \"EINK 250X128\" readable, no broken strokes\r\n");
    printf("  - 1px checkerboard clean, no random noise\r\n");
    printf("  - thin lines unbroken (broken = SPI timing)\r\n");
    printf("  - outer frame: if the bottom line is missing,\r\n");
    printf("    the visible height is <128 (2.13\" spec = 122)\r\n");
    printf("--------------------------------------------------\r\n\r\n");

    /* 之后只闪灯报活, 不再刷屏(墨水屏不需要持续刷新) */
    i = 0;
    while (1)
    {
        GPIO_ToggleBits(GPIOC, GPIO_Pin_3);
        Delay(1000);
        i++;
        if ((i % 10) == 0)
        {
            printf("alive %u s\r\n", i);
        }
    }
}

/**
  * @brief  Inserts a delay time.  (declared in main.h)
  */
void Delay(__IO uint32_t nTime)
{
    TimingDelay = nTime;
    while (TimingDelay != 0);
}

/**
  * @brief  Decrements the TimingDelay variable. Called from SysTick_Handler
  *         in Project/Test/stm32l1xx_it.c.
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
