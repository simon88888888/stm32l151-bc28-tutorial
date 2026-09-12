/**
  ******************************************************************************
  * @file    Project/Test/main.c
  * @brief   BC28 AT bridge: USART1 (CH340 / USB1) <-> USART2 (BC28 main UART).
  *
  *   WHY THIS EXISTS
  *   The BC28's MAIN uart is wired only to STM32 USART2 (PA2 = TX, PA3 = RX).
  *   There is no PC-side path to it: USB2/FT232 is not connected to MAIN (the
  *   SW8 switch routes FT232 to the module's DBG/AUX port instead). So to type
  *   AT commands at the module you must go *through* the MCU. This firmware
  *   does nothing but shovel bytes both ways, so the PC can talk to the modem.
  *
  *   PC SIDE   : 9600 8-N-1, flow control = none, on the CH340 COM port.
  *   MODEM SIDE: 9600 8-N-1 (same rate the vendor examples use for USART2).
  *
  *   NOTE ON POLLING vs INTERRUPTS
  *   The vendor's uart*_init() enables the RXNE interrupt and its handler reads
  *   DR into a buffer. That would steal the bytes from the main loop, so both
  *   RXNE interrupts are disabled right after init and the loop polls instead.
  *   Polling at 9600 baud is trivially fast enough and has no buffer race.
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usart.h"
#include "led.h"

/* These two live in HARDWARE/usart/usart.c but are not declared in usart.h. */
void UART1_send_byte(char data);
void UART2_send_byte(char data);

/* Private variables ---------------------------------------------------------*/
static __IO uint32_t TimingDelay;
static __IO uint32_t Heartbeat;

/* Private function prototypes -----------------------------------------------*/
void Delay(__IO uint32_t nTime);
void TimingDelay_Decrement(void);

/**
  * @brief  Main program.
  */
int main(void)
{
    /* 1 ms SysTick interrupt (TimingDelay_Decrement is called from it) */
    if (SysTick_Config(SystemCoreClock / 1000))
    {
        while (1);
    }

    LED_Init();          /* on-board LED on PC3 - used as a heartbeat */
    uart1_init(9600);    /* USART1 -> CH340 -> USB1   (the PC)      */
    uart2_init(9600);    /* USART2 -> BC28 MAIN uart                */

    /* Hand the bytes to the main loop instead of the vendor's ISRs. */
    USART_ITConfig(USART1, USART_IT_RXNE, DISABLE);
    USART_ITConfig(USART2, USART_IT_RXNE, DISABLE);

    printf("\r\n");
    printf("### BC28 AT bridge ###\r\n");
    printf("build: " __DATE__ " " __TIME__ "\r\n");
    printf("PC port: 9600 8-N-1, flow control = none. Type AT commands.\r\n");
    printf("Any byte you type goes to the module; its reply comes back here.\r\n");

    while (1)
    {
        /* PC -> module */
        if (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) != RESET)
        {
            UART2_send_byte((char)USART_ReceiveData(USART1));
            Heartbeat = 0;
        }

        /* module -> PC */
        if (USART_GetFlagStatus(USART2, USART_FLAG_RXNE) != RESET)
        {
            UART1_send_byte((char)USART_ReceiveData(USART2));
            Heartbeat = 0;
        }

        /* Heartbeat: slow blink while idle, so you can see the bridge is alive
           even when nothing is being typed. */
        if (++Heartbeat > 2000000U)
        {
            Heartbeat = 0;
            GPIO_ToggleBits(GPIOC, GPIO_Pin_3);
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
