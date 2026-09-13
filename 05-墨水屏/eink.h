/**
  ******************************************************************************
  * @file    HARDWARE/eink/eink.h
  * @brief   2.13" 单色墨水屏 (SSD1673) 最小驱动 —— 只为"看显示效果"
  *
  *  面板 RAM : 250 (栅极 Y) x 128 (源极 X)
  *             栅极 250 条沿 48.7mm 长边排 -> Y 轴 = 屏的横向
  *             源极 122 条沿 23.8mm 短边    -> X 轴 = 屏的纵向
  *             (本驱动按 128 排, 多出来的几条是屏外死区)
  *  屏尺寸   : 250 x 128, 可视约 250 x 122  (2.13" = 54.2mm 对角线, 像素间距 0.195mm)
  *  位约定   : 1 = 白, 0 = 黑
  *  帧缓冲   : fb[250][16] = 4000 字节, [栅极][源极字节], 每字节 8 个纵向像素, MSB 先出
  *  引脚     : 软件(位操作) SPI
  *             SCLK=PA5  SDA=PA7  CS=PA4  DC=PB1  RST=PB0  BUSY=PC5
  *             —— 跟厂家 smp.h 一致, 同一块板实测过
  *  与 BC28 不冲突: BC28 走 USART2(PA2/PA3), CH340 走 USART1(PA9/PA10)
  ******************************************************************************
  */

#ifndef __EINK_H
#define __EINK_H

#include "stm32l1xx.h"

#define EINK_W        250       /* 屏宽(像素) = 栅极数 */
#define EINK_H        128       /* 屏高(像素) = 源极数, 可视约 122 */

#define EINK_BLACK    0
#define EINK_WHITE    1

/* 初始化: 配 GPIO + 复位 + SSD1673 寄存器序列 + 擦白一次 */
void EINK_Init(void);

/* --- 画到帧缓冲(不碰屏), 全部要调 EINK_Refresh() 才上屏 --- */
void EINK_Clear(unsigned char color);
void EINK_SetPixel(unsigned int x, unsigned int y, unsigned char color);
void EINK_FillRect(unsigned int x0, unsigned int y0, unsigned int x1, unsigned int y1,
                   unsigned char color);
void EINK_Rect(unsigned int x0, unsigned int y0, unsigned int x1, unsigned int y1,
               unsigned char color);

/* --- "看效果"专用图元 --- */
void EINK_Checker(unsigned int x0, unsigned int y0, unsigned int x1, unsigned int y1,
                  unsigned int cell, unsigned char color);
void EINK_VLines(unsigned int x0, unsigned int y0, unsigned int x1, unsigned int y1,
                 unsigned int spacing, unsigned char color);
void EINK_HLines(unsigned int x0, unsigned int y0, unsigned int x1, unsigned int y1,
                 unsigned int spacing, unsigned char color);
/* level = 0..16, 4x4 有序抖动, 用来看只有黑白两色时的过渡能力 */
void EINK_Dither(unsigned int x0, unsigned int y0, unsigned int x1, unsigned int y1,
                 unsigned int level, unsigned char color);

/* --- 5x7 字库, scale=1/2/3 -> 字高 7/14/21 --- */
void EINK_Char(unsigned int x, unsigned int y, char ch, unsigned int scale);
void EINK_Text(unsigned int x, unsigned int y, const char *str, unsigned int scale);

/* --- 上屏 --- */
/* 全刷: 把帧缓冲推到面板。返回本次耗时(ms)。ok 非空则置 1=正常 / 0=BUSY 超时 */
unsigned int EINK_Refresh(unsigned char *ok);
/* 等 BUSY 释放(低=空闲)。1=正常 0=超时 */
unsigned char EINK_BusyWait(unsigned int timeout_ms);

#endif /* __EINK_H */
