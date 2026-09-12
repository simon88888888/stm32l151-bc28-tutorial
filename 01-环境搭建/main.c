/**
  ******************************************************************************
  * @file    Project/Test/main.c
  * @brief   Hello World over USART1 (CH340 / USB1).
  *
  *   printf() is redirected to USART1 by HARDWARE/usart/usart.c (fputc).
  *   PC serial port settings: 9600 8-N-1, flow control = none.
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usart.h"
#include "timer.h"
#include "led.h"

/* Private variables ---------------------------------------------------------*/
GPIO_InitTypeDef GPIO_InitStructure;
static __IO uint32_t TimingDelay;

/* Private function prototypes -----------------------------------------------*/
void Delay(__IO uint32_t nTime);
void TimingDelay_Decrement(void);

/**
  * @brief  Main program.
  */
int main(void)
{
    int count = 0;

    /* 1 ms SysTick interrupt - used by Delay() */
    if (SysTick_Config(SystemCoreClock / 1000))
    {
        while (1);
    }

    LED_Init();          /* on-board LED on PC3 */
    uart1_init(9600);    /* USART1 -> CH340 -> USB1 */

    printf("\r\n\r\n");
    printf("############ STM32L151 + BC28 ############\r\n");
    printf("############  Hello World    ############\r\n");
    printf("build: " __DATE__ " " __TIME__ "\r\n\r\n");

    while (1)
    {
        printf("Hello World  #%d\r\n", count);
        count++;

        GPIO_ToggleBits(GPIOC, GPIO_Pin_3);   /* blink LED so you can see it runs */
        Delay(1000);                          /* 1 s */
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
