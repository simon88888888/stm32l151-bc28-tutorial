# BC28 AT 透传固件（`main.c`）

**这个固件只做一件事**：把 USB1 收到的字节原样转给 BC28，把 BC28 回的字原样转回 USB1。
于是电脑上就能直接敲 AT 指令、看模块的原样回复。

配第 2 篇正文看：**[第2篇-BC28联网与AT指令实测.md](../第2篇-BC28联网与AT指令实测.md)**。
PC 端工具在 **[`02-工具/at_console.py`](../02-工具/at_console.py)**。

> 它**不是**厂家例程里那种应用固件 —— 那种一上电自己就 `AT+CGATT=1`、建 socket 发数据，
> 串口全被它自己占了。这个跑起来之后串口是"空"的，等你在 PC 上敲。

## 一、为什么非要这么一个东西（板子的硬约束，先看这个）

BC28 的 **MAIN 串口**（模块 17/18 脚）**只连到 STM32 的 USART2**（PA2=TX / PA3=RX，经
TXS0108E 电平转换 + R58/R59 0R）。板子上**没有 PC 直连 MAIN 的通路**：

| 板上的 USB 口 | 后面接的是 | 能不能敲 AT |
|---|---|---|
| **USB1** | CH340G → STM32 USART1 | ❌ 只能到 MCU，到不了模块 |
| USB2 | FT232(U12) → SW8 拨码 → 模块的 **DBG/AUX** | ❌ 不是 MAIN 口（DBG 口不吃普通 AT） |
| 核心板的 MINI_USB | 直连 BC28 的 USB_DP/DM | ❌ 只能用来升级固件（QFlash） |

**三个 USB 口，没有一个能直连 MAIN。** 所以 **AT 必须"穿过" MCU**，
就得有份只搬字节的固件在跑 —— 这就是本目录这份 `main.c`。

> 这一条是很多人卡住的地方：插上 USB1 打开串口助手敲 `AT`，一个字都不回，
> 就以为"模块坏了"或"卡没插好"。**其实那句 `AT` 根本没到模块跟前。**

## 二、数据通路

```
PC (at_console.py / 串口助手)
      │  9600 8-N-1, 无流控
   USB1 │ CH340G
      ▼
 STM32 USART1  ──►  main() 主循环  ──►  USART2  ──►  BC28 MAIN
      (轮询)          (双向搬字节)        (轮询)
```

两个方向都是 **9600 8-N-1**，速率在 `uart1_init(9600)` / `uart2_init(9600)` 里写死。

## 三、怎么建这个工程 / 怎么编译烧录

**建**：拿资料包的 **例程 1**（ST 的 `STM32L1xx_StdPeriph_Templates` 模板工程）拷到短路径 →
跑一遍 **`02-工具/prep_project.py`**（修那三处缺陷，见该目录 README）→
用本目录的 **`main.c` 覆盖 `Project\Test\main.c`** → 编译。

**可选的精简**（不精简也能编，只是多编译几个用不到的文件）：把 `.uvproj` 的 target 1 里
`Project/HARDWARE/{BC28,eink,TIMER}` 三个分组和 `Utilities\` 那几个 IncludePath 去掉。
本工程就是这么裁的 —— 从 67 MB / 4 个 target 裁到 **4 MB / 1 个 target**，
`.uvproj` 从 127303 字节到 **31417 字节**。裁剪前后差在哪，看 **`Project.uvproj.bak-strip`**
（在本工程 `Project\Test\MDK-ARM\` 下；**注意它不在本仓库**，本仓库只发 `main.c`）。

**烧**：把 `02-工具/flash_and_run.bat` 拷到 `Project\Test\MDK-ARM\` 下双击。
**判据：`flash.log` 里要有 `Application running ...`**（没有就是踩了 `-FO7` 那个坑）。

编译出来大概这个量级：

```
0 Error(s), 0 Warning(s)    Code=3088  RO-data=344  RW-data=72  ZI-data=2584
```

烧完串口会先打一段 banner，然后就是静默等下指令：

```
### BC28 AT bridge ###
build: Sep 12 2026 23:13:29
PC port: 9600 8-N-1, flow control = none. Type AT commands.
```

LED（PC3）在**空闲时慢闪**当心跳 —— 串口没动静时靠它判断固件在不在跑。

## 四、实现上三个必须知道的点

1. **轮询，不用中断。** 厂家的 `uart*_init()` 会打开 RXNE 中断，它的中断服务程序会把字节
   抢进自己的缓冲区，主循环就收不到了。所以 init 之后立刻
   `USART_ITConfig(USARTx, USART_IT_RXNE, DISABLE)`，主循环里轮询。
   9600 波特率下轮询绰绰有余，而且没有缓冲区竞争。
2. **`TimingDelay_Decrement()` 不能删。** `stm32l1xx_it.c` 的 `SysTick_Handler` 会调它。
   本固件用不上 `Delay()`，但这个函数必须留着，否则链接不过。
3. **`UART1_send_byte()` / `UART2_send_byte()` 在 `usart.c` 里有定义、但 `usart.h` 里没声明**，
   所以 `main.c` 顶上自己声明了一遍。

## 五、和厂家模板的差别（`main.c.diff`）

本目录的 **`main.c.diff`** 是 **`main.c.orig`（厂家那份，就是 ST 的
`STM32L1xx_StdPeriph_Templates/main.c`）→ `main.c`（这份透传）** 的完整 diff，
83 删 / 59 增。归纳起来：

| 删掉 | 为什么 |
|---|---|
| ST 模板里的时钟/外设初始化演示代码 | 透传只需要 USART1 + USART2 + 一个 LED |
| `Delay()` 调用链 | 用 SysTick 变量做心跳，不需要阻塞延时 |

| 加上 | 干什么 |
|---|---|
| `USART_ITConfig(..., USART_IT_RXNE, DISABLE)` | 把厂家的中断收字节关掉，改主循环轮询 |
| 双向搬字节的主循环 | 这就是"透传"本身 |
| `#include "usart.h"` 之外的 `UART*_send_byte()` 手工声明 | 见上面第 3 点 |
| banner 打印 + PC3 心跳 | 判断固件在不在跑 |

> `.uvproj` 那边另有三处改动（`IncludePath` 去掉 `Utilities`/`BC28`/`eink`/`TIMER`、
> 删掉 `BC28.c` 等三个文件项、删掉 target 2/3/4），**没放进仓库** ——
> 一是那三处缺陷的 diff 已经在 **[`03-工程改动/`](../03-工程改动/)** 里了，
> 二是"删 target 2/3/4"在 diff 里是几万行的删除块，放进来只会淹掉正文。
> 要照做的话按上面第三节的两句话操作即可。

## 六、一个别被绊到的地方

产物名还叫 **`STM32L152-EVAL`**（输出目录 `.\STM32L152-EVAL\`）—— 厂家留下的名字，
跟本板（STM32L151RC）对不上。**故意没改**：改它要连带动 `OutputDirectory` / `ListingPath` /
`.uvopt` 里的引用和烧录脚本，为个名字不值当。知道这回事就行。
