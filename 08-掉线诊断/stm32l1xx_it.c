/**
  ******************************************************************************
  * @file    Project/STM32L1xx_StdPeriph_Templates/stm32l1xx_it.c 
  * @author  MCD Application Team
  * @version V1.2.0
  * @date    16-May-2014
  * @brief   Main Interrupt Service Routines.
  *          This file provides template for all exceptions handler and 
  *          peripherals interrupt service routine.
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; COPYRIGHT 2014 STMicroelectronics</center></h2>
  *
  * Licensed under MCD-ST Liberty SW License Agreement V2, (the "License");
  * You may not use this file except in compliance with the License.
  * You may obtain a copy of the License at:
  *
  *        http://www.st.com/software_license_agreement_liberty_v2
  *
  * Unless required by applicable law or agreed to in writing, software 
  * distributed under the License is distributed on an "AS IS" BASIS, 
  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  * See the License for the specific language governing permissions and
  * limitations under the License.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include <stdio.h>              /* HardFault_Handler 要 printf 现场 */
#include "stm32l1xx_it.h"
#include "main.h"
#include "bc28.h"               /* RTC_WKUP_IRQHandler 要 BC28_WutClear() */

/** @addtogroup Template_Project
  * @{
  */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/

/******************************************************************************/
/*            Cortex-M3 Processor Exceptions Handlers                         */
/******************************************************************************/

/**
  * @brief  This function handles NMI exception.
  * @param  None
  * @retval None
  */
void NMI_Handler(void)
{
}

/**
  * @brief  硬件错误: 打一份现场, 停一下让日志发完, 然后自己复位。
  *
  *  2026-09-15 改。原来是厂家模板的 `while (1) {}` —— 一旦触发, 板子就
  *  **永远静默地卡在这儿**: 串口一个字没有、看板没数据、AT 也不回,
  *  断电重开还是查不出原因(当时不知道是它)。当天就真遇到过一次"跑着跑着
  *  突然不出声了", 只能靠一通排查才复位回来。
  *
  *  现在: 先把故障寄存器打出来 —— 有了 CFSR 就能知道是总线错误、取指错误、
  *  非对齐访问还是除零, 不用再猜 —— 停一下让 9600 波特的串口把话说完,
  *  然后 NVIC_SystemReset() 自己重启。**故障从"安静地死掉"变成"看得见 + 自愈"。**
  *
  *  注: 复位只重启 MCU, 不会重启 BC28 —— 那是应用起来后 BC28_Init() 里
  *  那句 AT+NRB 的事(见 bc28.c 的 BC28_Reset())。
  *
  *  CFSR 位含义(ARMv7-M 架构手册 B3.2.15): bit8 IBUSERR 取指总线错误 /
  *  bit9 PRECISERR 精确数据总线错误(地址在 BFAR) / bit10 IMPRECISERR 不精确写 /
  *  bit16 UNDEFINSTR 未定义指令 / bit24 UNALIGNED 非对齐访问 / bit25 DIVBYZERO 除零。
  */
void HardFault_Handler(void)
{
  volatile uint32_t *cfsr = (volatile uint32_t *)0xE000ED28;   /* 可配置故障状态 */
  volatile uint32_t *hfsr = (volatile uint32_t *)0xE000ED2C;   /* 硬故障状态 */
  volatile uint32_t *bfar = (volatile uint32_t *)0xE000ED38;   /* 出错的地址 */
  volatile uint32_t  i, j;

  printf("\r\n!! HARDFAULT\r\n");
  printf("   CFSR=%08lX  HFSR=%08lX  BFAR=%08lX\r\n",
         (unsigned long)*cfsr, (unsigned long)*hfsr, (unsigned long)*bfar);
  printf("   IBUSERR=%d PRECISERR=%d IMPRECISERR=%d UNDEFINSTR=%d"
         " UNALIGNED=%d DIVBYZERO=%d\r\n",
         (int)((*cfsr >> 8)  & 1u), (int)((*cfsr >> 9)  & 1u),
         (int)((*cfsr >> 10) & 1u), (int)((*cfsr >> 16) & 1u),
         (int)((*cfsr >> 24) & 1u), (int)((*cfsr >> 25) & 1u));
  printf("   resetting ...\r\n");

  /* 空转一小会儿(-O0 下大约几百毫秒), 够 9600 波特把上面三行发完。
     这里故意不调 BC28_Millis(): 故障状态下中断和时基都不保证还正常。 */
  for (i = 0; i < 200u; i++)
  {
    for (j = 0; j < 10000u; j++)
    {
    }
  }

  NVIC_SystemReset();       /* 回不来也没关系, 下面那句兜底 */

  for (;;)
  {
  }
}

/**
  * @brief  This function handles Memory Manage exception.
  * @param  None
  * @retval None
  */
void MemManage_Handler(void)
{
  /* Go to infinite loop when Memory Manage exception occurs */
  while (1)
  {
  }
}

/**
  * @brief  This function handles Bus Fault exception.
  * @param  None
  * @retval None
  */
void BusFault_Handler(void)
{
  /* Go to infinite loop when Bus Fault exception occurs */
  while (1)
  {
  }
}

/**
  * @brief  This function handles Usage Fault exception.
  * @param  None
  * @retval None
  */
void UsageFault_Handler(void)
{
  /* Go to infinite loop when Usage Fault exception occurs */
  while (1)
  {
  }
}

/**
  * @brief  This function handles SVCall exception.
  * @param  None
  * @retval None
  */
void SVC_Handler(void)
{
}

/**
  * @brief  This function handles Debug Monitor exception.
  * @param  None
  * @retval None
  */
void DebugMon_Handler(void)
{
}

/**
  * @brief  This function handles PendSVC exception.
  * @param  None
  * @retval None
  */
void PendSV_Handler(void)
{
}

/**
  * @brief  This function handles SysTick Handler.
  * @param  None
  * @retval None
  */
void SysTick_Handler(void)
{
	  TimingDelay_Decrement();
}

/******************************************************************************/
/*                 STM32L1xx Peripherals Interrupt Handlers                   */
/*  Add here the Interrupt Handler for the used peripheral(s) (PPP), for the  */
/*  available peripheral interrupt handler's name please refer to the startup */
/*  file (startup_stm32l1xx_xx.s).                                            */
/******************************************************************************/

/**
  * @brief  This function handles PPP interrupt request.
  * @param  None
  * @retval None
  */
/*void PPP_IRQHandler(void)
{
}*/

/**
  * @brief  RTC 唤醒定时器中断: 就是**把 MCU 从 STOP 里叫醒的那一下**。
  *
  *  ★★ 这个函数**必须存在**。startup_stm32l1xx_md.s 里
  *     `RTC_WKUP_IRQHandler` 是 [WEAK] 弱符号, 内容是 `B .` —— 一条死循环。
  *     不在这里把它盖掉, 板子一进 STOP 被唤醒就跳进那个死循环:
  *     **永远出不来, 串口一个字都没有, 断电重开才会好。**
  *     这是整个省电功能里最容易漏、后果最严重的一处。
  *
  *  这里只做一件事: **清挂起位**。两个都要清 ——
  *     RTC 的 WUTF 不清, 下一次 WFI 会立刻返回;
  *     EXTI 线 20 的挂起位不清, 同上。少了任何一个都表现为"睡了等于没睡"。
  *     更糟的是 WUTF 一直挂着的话, EXTI 线上再也形不成上升沿, 那就不是
  *     "睡不着"而是**醒不过来了**。
  *
  *  真正清的那一下在 bc28.c 的 wut_clear() 里 —— 那边会**显式解开 WPR**,
  *  因为没法确认 ISR 这几位是不是受写保护(库里的 RTC_ClearFlag 自己不解)。
  *  见那个函数的注释。
  *
  *  别在这里加 printf 或别的耗时动作: 它在唤醒的最前端, 这时候时钟还没重建
  *  (跑在 HSI 16MHz 或 MSI 2.1MHz 上), 串口波特率是错的, 打出来全是乱码;
  *  真正要报告的都在 BC28_SleepMs() 醒来之后那六步里做。
  */
void RTC_WKUP_IRQHandler(void)
{
  if (RTC_GetITStatus(RTC_IT_WUT) != RESET)
  {
    BC28_WutClear();
  }
}

/**
  * @}
  */ 


/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
