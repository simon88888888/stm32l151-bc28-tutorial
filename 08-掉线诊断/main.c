/**
  ******************************************************************************
  * @file    Project/Test/main.c
  * @brief   第 3 篇 · 把数据发到自己的服务器
  *
  *  上电 -> 注网 -> 建 socket -> 每 30 秒把一行信号数据发到自己的服务器。
  *
  *  跟第 2 篇的分工:
  *    第 2 篇是**人在 PC 上敲 AT**, 证明"这条链路能通";
  *    这一篇把那个过程**搬进固件**, 让它自己跑 —— 不用 PC, 也不用有人看着。
  *
  *  五个开关, 改完重编译重烧:
  *    MODE_LONG            1 = 长连接(默认) / 0 = 每轮重连, 用来量建连耗时
  *    BC28_SKIP_NSOCO_URC  在 bc28.h, =1 就是复现厂家那种"不等 URC 就发"
  *    REPORT_MS_DFLT       上报间隔的**出厂默认值**; 运行期由下行 #CFG 覆盖
  *    DHT11_SELFTEST       1 = 开机先连读 10 次温湿度(调传感器用) / 0 = 关
  *    SLEEP_SELFTEST       1 = 开机先做 50 次 5 秒的 STOP 唤醒自检(调省电用)
  *
  *  省电(第 4 步)靠的是**把两轮上报之间那段长等待换成 STOP**:
  *    主循环等 report_ms 的那一段 -> BC28_SleepMs() -> RTC 唤醒定时器倒数 ->
  *    进 STOP(电流从 mA 掉到 uA) -> 醒来重建 32MHz 时钟/接回 1ms 心跳/
  *    补回时基/清掉 UART 残留/探活模块。全在 bc28.c「四、STOP 低功耗」里。
  *  ★ **RTC 唤醒源起不来就绝不进 STOP**, 自动退回空转 —— 宁可费电不能睡死。
  *
  *  上行/下行协议 —— 看板上那个「正常/省电」开关就是靠这条链路生效的:
  *    -> #12,CSQ=..,UP=..,T=..,RM=30000,S=0,P=1      (末尾回显板子当前档位)
  *    <- AUTH OK / OK 12, 后面捎带一行 "#CFG <report_ms> <stop> <psm>"
  *  是**板子主动拉取**: 服务端在每条回执里都带配置(幂等), 板子下次上报时顺便
  *  取回。NB-IoT 上也只能这么做 —— 进了 STOP 就收不到字节, "随时能收命令"
  *  跟"省电"天生互斥。
  *
  *  ⚠ 配置**不写 Flash**, 拔电重插就回到 DFLT/0/0。这是故意的逃生门:
  *    配错了睡死, 拔电 2 分钟就能救回来; 写进 Flash 就变成永久砖。
  *
  *  串口: USART1 -> CH340 -> 电脑, 9600 8-N-1, 流控无
  *        (第 1 篇那套 flash_and_run.ps1 / read_com.ps1 直接就能看)
  *
  *  ⚠ **日志一律用英文。** AC5 在字符串字面量里遇到中文会报
  *    warning #870-D: invalid multibyte character sequence ——
  *    注释里可以写中文(注释在编译前就被剥掉了), 打进串口的字符串不行。
  ******************************************************************************
  */

/* ---- 实验二 的 A/B 开关 ---------------------------------------------------
   ⚠ 必须定义在 `#include "bc28.h"` **之前** —— bc28.h 里那一段是 #ifndef 保护的,
     先定义才能盖住它的默认值 0。

   0 = 正常写法: 发完 AT+NSOCO 之后必须等到 "+NSOCO: <n>" 这条 URC 才算连上。
   1 = 复现**厂家**的写法: 收到 OK 就当成连上了, 立刻发数据。
       (厂家 BC28_ConTCP() 整个文件里没出现过 "+NSOCO" 这个串。)

   第 3 篇 §6 的对照实验就是只改这一个数、重编译、重烧 —— 同一份固件、
   同一个服务器、同一个时间段, 只差"等没等那条 URC"。 */
#define BC28_SKIP_NSOCO_URC     0

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usart.h"
#include "led.h"
#include "bc28.h"
#include "dht11.h"
#include <stdio.h>
#include <string.h>

/* ---- 你的服务器 -----------------------------------------------------------
   **写域名比写 IP 好。** 家宽的出口 IP 是动态的, 光猫重启一次就变; 写死 IP
   意味着每次变了都要重编译重烧 —— 那就等于没法长期挂着。

   固件自己判断这一行是哪种:
     - 看着像 IPv4(四段数字) -> 直接用, 一次 AT 都不多发
     - 否则当域名           -> 先用**模块自带的 AT+QDNS** 解析成 IP 再连

   好消息是这件事不用自己做: 模块自带 DNS 解析, 所以
     - 固件里不需要塞 DNS 客户端(几百行, 还要自己处理 UDP 重传)
     - 不需要给模块开 IPv6(移动 NB-IoT 本来也没给模块分 v6)
     - 不需要额外挂 DDNS 客户端, 只要域名那边有个**带 A 记录**的 DDNS 就行

   ⚠ 公开仓库里这一行必须是占位符 —— 别把自己的服务器地址提交上去。
     真值只留在本机这份工程里。 */
#define SERVER_HOST     "your-server.example.com"
#define SERVER_PORT     9101

/* 预共享 token。**真值只在服务器的 sink.env(600) 里, 不进任何仓库。**
   服务端每条连接的第一行必须是 "#TOKEN <它>", 不对就记一条 REJECT 然后断开。
   为什么公网裸端口一定要有这个 —— 见第 3 篇 §2。 */
#define SINK_TOKEN      "put-your-own-token-here"

/* ---- 开关 -----------------------------------------------------------------*/
/* 2026-09-14 实测: 当前这条 NB-IoT 路径上, 长连接**只过得去 1 条**就静默失效 ——
   服务端 ss 查不到这条连接、模块却继续对 AT+NSOSD 回 OK, 板子一直发、一直收不到应答;
   换成第 3 篇那版固件(不含 DHT11)复现完全一样, 所以跟传感器无关。
   短连接同一晚实测 43/43 全中。先把第 5 篇跑通, 长连接的自动重连单独做一次改动。 */
#define MODE_LONG       0               /* 1 = 长连接(推荐); 0 = 每轮建连->发->收->关 */

/* 上报间隔。**现在只有 DFLT 是编译期常量** —— 真正用的是下面那个 report_ms,
   每次收到服务端的 #CFG 就被改写。
   两个界限值故意跟服务端 nbcfg.py 里的 REPORT_MS_MIN/MAX 写成一样:
   服务端已经夹过一遍, 板子这里**独立再判一遍**(纵深防御)。
   区别是板子侧**整条丢掉**而不是夹紧 —— 理由见 cfg_scan() 上面那段。 */
#define REPORT_MS_DFLT  30000UL
#define REPORT_MS_MIN   10000UL
#define REPORT_MS_MAX   3600000UL

/* 开机先连读 10 次温湿度, 专门用来**调传感器**。
   调的时候把它开成 1: 读 10 次、每次隔 2 秒 = 20 秒, 正好一次
   read_com.ps1 -Seconds 25 的量 —— 一次烧录换 20 个数据点, 省掉反复烧写。
   调通了改成 0 重烧, 温湿度就并进每 30 秒那条上报里。 */
#define DHT11_SELFTEST  0

/* 开机先做 50 次 5 秒的 STOP 自检, 专门用来**调唤醒**。
   ★ 只在调 STOP 的时候开成 1, 调完必须改回 0 重烧 —— 这个自检要跑 4 分多钟,
     而且它会把正式流程整个推迟。

   为什么值得单独有这么个自检: STOP 出问题的**每一种**死法都长得一样 ——
   "板子不说话了"。而原因可能是时钟没重建、SysTick 没接回来、时基没补、
   UART 残留没清、模块没醒、`RTC_WKUP_IRQHandler` 没定义(跳到 startup 里
   那个 `B .` 死循环)。50 次连跑 + 每轮重新量一次 Delay(1000),
   能把这些**逐个**分开: 只要有一轮 Delay(1000) 不是 1000, 就是时钟/时基那一侧;
   全是 1000 但"睡"了 0 毫秒, 就是 RTC 那一侧。 */
#define SLEEP_SELFTEST  0

#if MODE_LONG
#define MODE_NAME       "long"
#else
#define MODE_NAME       "short"
#endif

/* Private variables ---------------------------------------------------------*/
static __IO uint32_t TimingDelay;

static int      sock = -1;      /* 当前 socket, -1 = 没连上 */
static uint32_t seq;            /* 上报序号, 从 1 开始 */
static uint32_t rx_ok;          /* 服务器回了 OK 的次数 */
static uint32_t rx_lost;        /* 发出去了但没等到 OK 的次数 */

/* 温湿度。**必须 static** —— 本工程 Stack_Size 只有 0x400 = 1KB,
   DHT11_DATA 里光实测量就有十几个 uint16, 放栈上不合适。
   而且它是**上一轮**的读数, 天生就该是跨函数存在的状态。 */
static DHT11_DATA dht;          /* 最近一次读数(失败时里面的实测量仍然有效) */
static uint32_t   dht_ms;       /* 上次读的时刻, 用来限流 */
static int        dht_rc;       /* 最近一次读的返回值 */

static uint32_t   conn_fail;    /* 连续建连失败了几次 —— 到阈值就重启模块 */

/* 连续几次 "注着网却一个 socket 号都开不出来" —— **这才是真漏光的指纹**。
   见主循环 ④-0 那段, 那里解释了为什么不能拿 socket 账本的余额当判据。 */
static int        nosock_run;

/* ---- 下行配置(服务端 #CFG) ----
   ★ **故意不写 Flash**, 理由见文件头那段。这里再写一遍是要挡住"顺手加上":
     加持久化 = 新写一套 Flash 驱动 + 页擦除 + 掉电保护, 而这个工程里
     一行 Flash 代码都没有; 换来的却是"配错就永久变砖"。不值。

   stop 已经在第 2 批**真执行了**(就是进 STOP, 由主循环里的 `if (cfg_stop)` 闸住);
   psm 仍然只是"收下 + 回显", 真发 AT+CPSMS 是第 3 批的事。
   分两批做是因为合在一起之后板子要是不说话, 就分不清是解析写坏了还是
   STOP 没醒。 */
static uint32_t report_ms = REPORT_MS_DFLT;  /* 运行期上报间隔 */
static int      cfg_stop;                    /* 服务端要的 STOP 档(第 2 批执行) */
static int      cfg_psm;                     /* 服务端要的 PSM 档 (第 3 批执行) */
static uint32_t cfg_ver;                     /* 收到过几条合法 #CFG */

#if !MODE_LONG
/* 短连接模式的建连耗时统计(长连接那一档用不到, 不声明就不会有 unused 警告) */
static uint32_t conn_n, conn_min, conn_max, conn_sum;
#endif

/* Private function prototypes -----------------------------------------------*/
void Delay(__IO uint32_t nTime);
void TimingDelay_Decrement(void);

static int  put_tenth(char *p, int v);
static void trim(char *s);
static void dht_temp_str(char *out, const DHT11_DATA *d);
static int  wait_reply(int s, char *out, uint16_t cap, int rounds);
static int  cfg_num(const char *s, uint32_t *out);
static int  cfg_accept(uint32_t rms, int st, int ps);
static int  cfg_scan(const char *buf);
static int  ack_is_data(const char *buf);
static int  link_up(void);
static void report_once(void);
static int  hard_recover(void);
static void connect_failed(uint32_t wait_ms, int why);

/**
  * @brief  把一个"单位 0.1"的整数打成人看的样子: -683 -> "-68.3", 64 -> "6.4"。
  *         AT+NUESTATS 的 dB 类字段单位就是 0.1(第 2 篇 §4.4), 别当整数用。
  */
static int put_tenth(char *p, int v)
{
    char *s = p;

    if (v < 0)
    {
        *p++ = '-';
        v = -v;
    }
    p += sprintf(p, "%d.%d", v / 10, v % 10);
    return (int)(p - s);
}

/** @brief  去掉行尾的 \r\n, 免得打日志时把版面拆了。 */
static void trim(char *s)
{
    size_t n = strlen(s);

    while ((n > 0) && ((s[n - 1] == '\r') || (s[n - 1] == '\n') || (s[n - 1] == ' ')))
    {
        s[--n] = '\0';
    }
}

/**
  * @brief  把 DHT11 的温度打成人看的样子: "24.0" / "-5.0"。
  *
  *         ⚠ DHT11 温度**整数部分的最高位是符号位**(负温时置 1), 不是无符号数。
  *           直接按无符号打的话, -5 度会变成 251 度 —— 数据手册第 6 页那张
  *           位定义表里写了, 但很容易漏掉。湿度那两字节没有符号位, 不用管。
  *
  *         小数部分 DHT11 恒为 0(它的分辨率就是 1 度), 留着是为了跟
  *         DHT22 的格式对齐 —— 以后换传感器上层不用改。
  */
static void dht_temp_str(char *out, const DHT11_DATA *d)
{
    int t   = (int)d->temp;
    int neg = 0;

    if ((t & 0x80) != 0)
    {
        neg = 1;
        t  &= 0x7F;
    }
    sprintf(out, "%s%d.%d", (neg != 0) ? "-" : "", t, (int)d->temp_dec);
}

/**
  * @brief  看着像 IPv4 吗?  "203.0.113.7" 是, "your-server.example.com" 不是。
  *         只判形状(四段十进制数), **不校验数值范围** —— 够用, 也省得引 inet_aton
  *         那套要额外缓冲的东西。判错了也不危险: 顶多多走一次 DNS。
  */
static int is_ipv4(const char *s)
{
    int part = 0;
    int digits = 0;

    if (s == NULL)
    {
        return 0;
    }
    for (; *s != '\0'; s++)
    {
        if ((*s >= '0') && (*s <= '9'))
        {
            digits++;
            if (digits > 3)
            {
                return 0;               /* 一段超过 3 位数字, 不是 */
            }
        }
        else if (*s == '.')
        {
            if (digits == 0)
            {
                return 0;               /* 空段, 像 "1..2" */
            }
            part++;
            digits = 0;
            if (part > 3)
            {
                return 0;
            }
        }
        else
        {
            return 0;                   /* 出现字母/连字符 -> 是域名 */
        }
    }
    return ((part == 3) && (digits > 0));
}

/**
  * @brief  得到真正要连的 IP 字符串: 写的是 IP 就原样用, 写的是域名就先解析。
  *         返回 0 成功, < 0 = BC28_Resolve 的错误码。
  */
static int resolve_target(char *out, uint16_t cap)
{
    char buf[64];
    int  rc;

    if ((out == NULL) || (cap < 8))
    {
        return BC28_E_ARG;
    }

    if (is_ipv4(SERVER_HOST))
    {
        /* 写的就是 IP —— 省掉一次 AT 往返。这一支就是没上 DDNS 时的样子。 */
        strncpy(out, SERVER_HOST, (size_t)(cap - 1));
        out[cap - 1] = '\0';
        return 0;
    }

    rc = BC28_Resolve(SERVER_HOST, buf, sizeof(buf));
    if (rc < 0)
    {
        printf("! DNS: cannot resolve %s: %s\r\n", SERVER_HOST, BC28_Why(rc));
        return rc;
    }
    printf("+ DNS: %s -> %s   (module's own AT+QDNS)\r\n", SERVER_HOST, buf);

    strncpy(out, buf, (size_t)(cap - 1));
    out[cap - 1] = '\0';
    return 0;
}

/**
  * @brief  读服务端回的一行(OK <n> / AUTH OK)。
  *         AT+NSORF 要**轮询**: 数据什么时候到是另一回事, 一条读不到就再读。
  *         返回字节数, 0 = 这几轮都没有。
  */
static int wait_reply(int s, char *out, uint16_t cap, int rounds)
{
    int i;
    int n;

    for (i = 0; i < rounds; i++)
    {
        n = BC28_Recv(s, out, cap, BC28_TO_NSORF);
        if (n > 0)
        {
            trim(out);
            return n;
        }
        if ((n < 0) && (n != BC28_E_TIMEOUT))
        {
            return n;               /* 真错误, 不装了 */
        }
        BC28_Idle(1000);
    }
    return 0;
}

/* ======================= 下行配置 (#CFG) ==================================
   看板上那个「正常 / 省电」开关, 走的就是下面这一小段。

   服务端在**每条回执**里都捎带一行配置(幂等, 丢了不心疼):
       AUTH OK\n#CFG 30000 0 0\n        握手回执
       OK 12\n#CFG 300000 1 1\n         每条数据的 ACK
   三个数: report_ms / stop / psm。正常档 = 30000 0 0, 省电档 = 300000 1 1。

   为什么要"每条都带"而不是"变了才带" —— 因为板子这边**不记账**:
   不知道服务端上次说的是什么, 也没有"我收到了吗"的回执。每条都带,
   任何一条漏了、乱了, 下一条就自动纠正回来。板子因此可以很笨:
   每次读到就无条件采用, 不需要序号、不需要重传、不需要状态机。
   ========================================================================= */

/**
  * @brief  从字符串开头读一串十进制数字。
  *         返回吃掉的字符数(>= 1), 0 = 这里压根不是数字。
  *
  *  ⚠ 不用 atoi/strtoul: 它们对 "abc" 回 0、对超范围的值夹到 ULONG_MAX,
  *     这两种都**分不出"字段是坏的"**。下行配置宁可不认, 不可认错 ——
  *     认错一个 report_ms 就是板子从此按错周期说话。
  */
static int cfg_num(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int      n = 0;

    while ((*s >= '0') && (*s <= '9'))
    {
        if (v > 400000000UL)        /* 再乘 10 就要溢出 uint32 了, 直接判坏 */
        {
            return 0;
        }
        v = (v * 10UL) + (uint32_t)(*s - '0');
        s++;
        n++;
    }

    if (n == 0)
    {
        return 0;
    }

    *out = v;
    return n;
}

/**
  * @brief  采用一条**已经判过合法性**的 #CFG。
  *         返回 1 = 跟当前不一样(值得打一行日志), 0 = 一样(闭嘴)。
  */
static int cfg_accept(uint32_t rms, int st, int ps)
{
    int changed;
    int first;

    changed = (rms != report_ms) || (st != cfg_stop) || (ps != cfg_psm);
    first   = (cfg_ver == 0);

    report_ms = rms;
    cfg_stop  = st;
    cfg_psm   = ps;
    cfg_ver++;

    if (first)
    {
        /* 开机第一条 —— 打出来是为了跟"服务端到底下发了吗"对账。
           这一行出现 = 下行链路是通的, 后面再出问题就不用怀疑这一侧。 */
        printf("+ CFG from server (#1): report=%lu ms  stop=%d  psm=%d"
               "  (downlink OK)\r\n",
               (unsigned long)report_ms, cfg_stop, cfg_psm);
    }
    else if (changed)
    {
        /* 这一行就是看板点开关之后要在串口里看到的东西 */
        printf("+ CFG applied (#%lu): report=%lu ms  stop=%d  psm=%d\r\n",
               (unsigned long)cfg_ver,
               (unsigned long)report_ms, cfg_stop, cfg_psm);
    }

    return changed;
}

/**
  * @brief  从一整段回执里找 "#CFG <report_ms> <stop> <psm>" 并采用它。
  *         返回 1 = 采用了, 0 = 这段里没有(合法的)。
  *
  *  ⚠ 扫**整段**缓冲, 不是只看第一行: 服务端是把 "AUTH OK" 和 "#CFG"
  *     拼在同一个 write 里发出来的(实测抓包确认是一个 TCP 段), 但板子
  *     这边**不该假设**这一点 —— 万一哪天中间多一跳、或者服务端改了写法
  *     拆成两次发, 只看第一行就整条漏掉。扫整段两边都不怕。
  *
  *  ★ 三个数必须**全部**解析成功、且都合法, 否则**整条丢掉**。
  *    这里**故意不夹紧**(不把越界的 report_ms 掰回 10000):
  *    夹紧会把"一个明显是坏的值"变成一个"看着很正常的值", 下次再想查
  *    就一点线索都没有了; 而丢掉不疼 —— 反正每条 ACK 都带配置。
  */
static int cfg_scan(const char *buf)
{
    const char *p = buf;
    uint32_t    rms;
    uint32_t    st;
    uint32_t    ps;
    int         n;

    if (buf == NULL)
    {
        return 0;
    }

    while ((p = strstr(p, "#CFG ")) != NULL)
    {
        p += 5;

        n = cfg_num(p, &rms);
        if (n == 0) { continue; }
        p += n;
        if (*p != ' ') { continue; }
        p++;

        n = cfg_num(p, &st);
        if (n == 0) { continue; }
        p += n;
        if (*p != ' ') { continue; }
        p++;

        n = cfg_num(p, &ps);
        if (n == 0) { continue; }
        p += n;

        /* 三个数都到手了, 现在才判合法。板子侧独立再来一遍限幅,
           跟服务端 nbcfg.py 里那套是对称的。 */
        if ((rms < REPORT_MS_MIN) || (rms > REPORT_MS_MAX)) { continue; }
        if (st  > 1) { continue; }
        if (ps  > 1) { continue; }

        (void)cfg_accept(rms, (int)st, (int)ps);
        return 1;
    }

    return 0;
}

/**
  * @brief  这一段回执里, **有没有"这一条上报的 ACK"**?
  *         判据: 某一行以 "OK " 开头、后面紧跟数字。
  *
  *  ★ 这是替换掉老代码里 `strcmp(buf, "AUTH OK") == 0` 那个判据的。
  *    老判据是用来"跳过迟到的握手回执"的(第 3 篇 §8 记的 lost 虚高那件事) ——
  *    但从服务端开始回 "AUTH OK\n#CFG ..." 那一刻起, 那个 strcmp **永远不成立**,
  *    于是迟到的握手回执会被当成这一轮的 ACK: rx_ok 虚高, 而真正的 ACK
  *    还留在 socket 里没人读。这就是第 3 篇那个坑的续集, 同一颗雷换个引信。
  *
  *    新判据两边分得开: "AUTH OK" 以 'A' 开头, 天生不满足。
  *
  *  ⚠ 下面这串 && 是**短路**的, 所以 p[1]/p[2]/p[3] 不会被越界读到:
  *    要读到 p[3], 必须先满足 p[0..2] 都不是 '\0', 也就是后面至少还有 3 个
  *    字符 —— 那 p[3] 最坏也就是那个 '\0' 本身, 仍在数组里面。
  */
static int ack_is_data(const char *buf)
{
    const char *p = buf;

    if (buf == NULL)                /* 跟 cfg_scan 一样兜一手, 现在没有调用者
                                       会传 NULL, 但这对函数是一起用的 */
    {
        return 0;
    }

    while (*p != '\0')
    {
        if (((p == buf) || (p[-1] == '\n') || (p[-1] == '\r')) &&
            (p[0] == 'O') && (p[1] == 'K') && (p[2] == ' ') &&
            (p[3] >= '0') && (p[3] <= '9'))
        {
            return 1;
        }
        p++;
    }

    return 0;
}

/**
  * @brief  最后一招: 让 BC28 整个重启, 然后等它重新注网。
  *
  *  为什么值得下这么重的手 —— 2026-09-14 实测: 模块的 socket 层会自己卡死,
  *  卡死时 AT+NSOCR 直接回 ERROR, 而**同一时刻 CSQ=25 / RSRP=-73**, 无线好得很。
  *  也就是说单看信号永远查不出来, AT 层的其它补救(NSOCL 等)也全被拒 ——
  *  只有 NRB 能把它救回来。详见 bc28.c 里 BC28_Reset() 的文件头注释。
  *
  *  返回 0 = 救回来了, < 0 = 重启了还是没注上网。
  */
static int hard_recover(void)
{
    if (conn_fail > 0)
    {
        printf("! %lu connects in a row failed -- module socket layer looks wedged\r\n",
               (unsigned long)conn_fail);
    }
    printf("! rebooting BC28 (AT+NRB), this takes ~2 min (re-search) ...\r\n");

    conn_fail = 0;
    BC28_Reset();

    /* ★ 这里原来是 BC28_WaitNet(60000) —— 只给 60 秒, 而 NRB 之后模块要从头
       搜网**约 2 分钟**(2026-09-16 实测 121 秒)。于是这个判断必然失败, 永远打印
       "rebooted but still not attached"; 然后 connect_failed 隔 3 秒再来 5 次、
       又 NRB —— 模块每次刚搜到一半就被打断, **永远注不上网**。

       2026-09-17 22:30~22:32 的串口实录就是这个指纹:
           net[1..6] CEREG stat=2      (CSQ 25~26, 信号好得很)
           ! rebooted but still not attached
           + DNS: ... -> FAIL  /  NSOCR: modem-ERROR
           ! rebooting BC28 (AT+NRB)   <- 距上一次 NRB 只隔了 112 秒

       跟启动那圈重试是**同一个 bug**(那边是 120000 卡在边界上), 所以用同一个
       常量, 以后只需调 BC28_TO_NET 一处。 */
    if (BC28_WaitNet(BC28_TO_NET) != 0)
    {
        printf("! rebooted but still not attached\r\n");
        return -1;
    }

    printf("+ BC28 rebooted and attached again\r\n");
    return 0;
}

/**
  * @brief  建连失败的统一出口: 记账、必要时重启模块、然后等一会儿。
  *         两个模式(MODE_LONG)都走这里, 免得同一套逻辑写两遍。
  */
static void connect_failed(uint32_t wait_ms, int why)
{
    /* ★★ 2026-09-20 改: 这里原来是**无条件** conn_fail++ —— 于是"一次
       AT+NSOCO 网络超时"被数成了"socket 层卡死", 攒够 5 次就 NRB。

       整夜实测(2026-09-19 22:10 ~ 2026-09-20 03:47)把这条钉死了:
         170 次建连失败里 **114 次是 AT+NSOCO 卡满 30 秒**（正好 = BC28_TO_URC）。
         可**成功建连的中位耗时只有 3 秒、最慢 16 秒** —— 30 秒那一档是另一个
         世界: 模块压根没回 +NSOCO URC。而且**其中 109 次发生在 RSRP -69 的
         好小区上**（同夜那个 -101 的弱小区只占 5 次）—— 跟无线好坏无关。
         这 170 次全被 conn_fail 数了进去, 触发 **19 次 NRB**, 每次离线
         190~240 秒, 整夜 **38% 的时间离线**（268 分钟 / 11.8 小时）。

       ★ 真正的指纹只有一个, 而且 BC28_Open() 早就把它分出来了:
         **注着网, 却开不出号**（BC28_E_NOSOCK）。
       网络超时(BC28_E_TIMEOUT) / 模块回 ERROR(BC28_E_ERROR) / DNS 解析失败
       都是**会自己好**的东西 —— 一次都不该攒。

       (旁证: 这一夜 "NSOCL failed" 与 "NSOCR failed:" 在串口里的次数是 114 : 0
        —— 也就是说下面那条正确的判据(主循环的 nosock_run)整夜一次都没触发过。
        **正确的尺子一次没用上, 错误的尺子用了 19 次**, 和第 7 篇 §2.4 同一个病。) */
    if (why == BC28_E_NOSOCK)
    {
        conn_fail++;            /* 只有"开不出号"才攒 */
    }
    else
    {
        conn_fail = 0;          /* 会自己好的, 不攒 */
    }

    printf("- connect failed: %s (%lu in a row), retry in %lu ms\r\n",
           BC28_Why(why),
           (unsigned long)conn_fail, (unsigned long)wait_ms);

    if (conn_fail >= 5)
    {
        /* ★★ 只有"注上网了却连不上"才值得 NRB —— 那才是 socket 层卡死。
           压根没注网的时候 NRB 只会**把它刚搜到一半的网打断**。

           2026-09-17 23:41~23:53 的串口实录就是一个**闭合的死循环**:
               23:41:15  ! rebooting BC28 (AT+NRB)          ← 保险丝
               23:46:49  ! rebooted but still not attached  ← 300 秒等到头
               23:46:52  ! NSOCO: ... modem-ERROR           ← 没注网, NSOCR 直接 ERROR
               ...(10 秒里连着 5 次, 每次都是瞬间失败, 一次 AT 都没真发出去)
               23:47:03  ! rebooting BC28 (AT+NRB)          ← 又 NRB, 一圈 5 分钟
               23:52:37  ! rebooted but still not attached
               23:52:55  ! rebooting BC28 (AT+NRB)
           **模块一次都没得到足够时间**。重新搜网实测要 2~8 分钟(最快 76 秒,
           最慢量到 7.5 分钟), 而这里每 5 分钟就打断它一次。

           注意这跟之前那个 60 秒的 bug 是**同一个病**: 把超时从 60 提到 300
           并没有打断循环, 只是让它每圈慢一点。真正的修法只有一条 ——
           **把"没注网"和"建连失败"彻底分开**: 没注网就安静地等它搜完,
           不数 conn_fail, 不发 NRB。 */
        if (BC28_WaitNet(0) != 0)
        {
            printf("! 5 fails in a row, but the module is NOT attached --\r\n");
            printf("! waiting for the network instead of NRB (NRB would eat the search)\r\n");
            conn_fail = 0;                  /* 这不算"建连失败", 别记在它头上 */

            /* 等它注上, 最长 10 分钟。这个上限是按"实测最慢 7.5 分钟"定的, 留了余量。
               到了还没注上就返回, 让主循环再走一圈 —— 依然**不会** NRB。 */
            (void)BC28_WaitNet(600000);
            return;                          /* 上面已经等够了, 不用再 BC28_Idle */
        }

        (void)hard_recover();
    }

    /* (注: hard_recover 里那句"几连败"只在真的连败时才打 —— 现在还有第二个
       调用者: 主循环的 socket 账本保险丝, 它是**主动**重启的, conn_fail 是 0,
       打出来会是"0 connects in a row failed", 反而误导。) */

    BC28_Idle(wait_ms);
}

/**
  * @brief  建一条到服务器的连接, 并把 token 交上去。
  *         返回 socket 号(>= 0), 或负错误码。
  */
static int link_up(void)
{
    char buf[64];
    char ip[64];
    int  s;
    int  rc;

    s = BC28_Open();
    if (s < 0)
    {
        printf("! NSOCR failed: %s\r\n", BC28_Why(s));
        return s;
    }

    /* 每次建连前都重新解析一次 —— 这正是"写域名"的价值所在:
       家里出口 IP 变了, 下一次重连自然就拿到新的, 固件一个字节都不用动。
       (所以解析放在 link_up 里、而不是开机时算一次存起来。) */
    rc = resolve_target(ip, sizeof(ip));
    if (rc < 0)
    {
        (void)BC28_Close(s);
        return rc;
    }

    rc = BC28_Connect(s, ip, SERVER_PORT, BC28_TO_URC);
    if (rc < 0)
    {
        printf("! NSOCO: no +NSOCO URC: %s\r\n", BC28_Why(rc));
        BC28_DumpRx();
        (void)BC28_Close(s);
        return rc;
    }

#if (BC28_SKIP_NSOCO_URC == 0)
    printf("+ TCP connected %s:%u  socket=%d  (+NSOCO URC came after %lu ms)\r\n",
           ip, (unsigned)SERVER_PORT, s,
           (unsigned long)BC28_NsocoDelayMs());
#else
    /* 这一档不等 URC, 所以此刻**根本不知道有没有真连上** —— 这正是要害 */
    printf("+ NSOCO got OK, assuming connected (socket=%d) -- vendor style\r\n", s);
#endif

    /* 握手: 每条连接的第一行必须是 token。
       ⚠ 行尾的 \r\n **不能省**。服务端是 readline() 按行读的, 少这一个换行,
       它会一直阻塞等下去, 从外面看就像"数据根本没到" —— 我们在第 3 篇实测里
       真踩了这个坑: 抓包显示每个字节都到了、内核也 ACK 了, 但服务端一条
       日志都没有。定位花了很久, 因为现象跟"链路不通"一模一样。 */
    rc = BC28_Send(s, "#TOKEN " SINK_TOKEN "\r\n",
                   (uint16_t)strlen("#TOKEN " SINK_TOKEN "\r\n"));
    if (rc < 0)
    {
        printf("! send token failed: %s\r\n", BC28_Why(rc));

        /* ★ 这一句原来没有 —— 是 link_up 里唯一一条**漏 socket** 的出口。
           上面三条失败路径(NSOCR 失败 / 解析失败 / NSOCO 失败)都关了, 只有这条
           直接 return, 那个号就永远留在模块里了。caller 那边 handle 不了:
           它看到负数只知道自己没拿到 socket, 根本不知道里面已经分了一个。

           平时这条出口很少走到(发 token 失败 = 链路刚建立就哑了),
           但**少走不等于不走** —— 而 socket 只有 6 个, 漏满就整块板子卡死,
           代价和概率完全不成比例。 */
        (void)BC28_Close(s);
        return rc;
    }

    /* 等不到 AUTH OK 也**不算连接失败**: 照发不误。
       理由有两个 —— 一是 NB-IoT 这条链路本来就慢, 二是 A/B 对照那一档
       故意不等, 如果这里一失败就重连, 就永远走不到"发数据"那一步,
       也就看不到"服务器到底收到什么"这个结果了。 */
    if (wait_reply(s, buf, sizeof(buf), 2) > 0)
    {
        printf("+ server: %s\r\n", buf);
        /* 握手回执里也带着配置, 而且这是**最早**能拿到服务端档位的地方 ——
           比第一条数据 ACK 还早。这一句别漏: 漏了会让"开机后第一轮还在用
           旧间隔"变成一个很难查的怪现象(要等到第二轮才纠正过来)。 */
        (void)cfg_scan(buf);
    }
    else
    {
        printf("! no AUTH OK (noted, going on anyway)\r\n");
    }
    return s;
}

/**
  * @brief  发一条上报, 并读服务端的应答。
  *         一行的样子: #12,CSQ=28,RSRP=-68.3,SNR=6.4,ECL=0,UP=360,T=26.9
  *
  *         ⚠ **只发温度, 不发湿度。** 手上这颗传感器的湿度通道**恒为 0**,
  *         而校验和每次都是对的 —— 也就是"不是读错位, 是它真在发 0"
  *         (详见下面自检那一段的注释)。房间里不可能有 0%RH, 所以那一栏发出去
  *         就是往服务端灌假数据, 不如不发。以后换一颗好的(或 DHT22),
  *         把 `,H=` 加回来就行。
  *
  *         温度读失败时发 -99.0 并补一个 DHTE=<错误码> —— **保持字段形状
  *         不变**。服务端是按逗号切、按 `=` 取值的, 少一栏或者变个形状都会
  *         让它解析出错; 而"这天传感器坏了"本身也是值得记下来的数据,
  *         不该让整条上报发不出去。
  */
static void report_once(void)
{
    BC28_RADIO r;
    char       body[128];
    char       buf[64];
    char       ts[16];
    char      *p;
    int        rc;
    int        n;
    int        i;
    int        got_ack;         /* 真拿到"这一轮的 ACK"了吗 —— 见下面那段注释 */

    /* ---- 温湿度 ----
       限流到 DHT11_MIN_PERIOD_MS: 一来手册要求采样间隔 >= 1 秒(敏感元件
       要缓过来, 连着读只会拿到同一个数), 二来读一次要**阻塞 20ms**。
       现在上报是 30 秒一轮, 所以实际上每轮都读; 这个判断是防以后把
       上报间隔调小的 —— 那时候它会自己变成"每 2 秒才读一次"。 */
    if ((uint32_t)(BC28_Millis() - dht_ms) >= DHT11_MIN_PERIOD_MS)
    {
        dht_rc = DHT11_Read(&dht);
        dht_ms = BC28_Millis();
    }

    memset(&r, 0, sizeof(r));
    if (BC28_GetRadio(&r) < 0)
    {
        r.csq = -1;
        printf("! radio read failed (sending anyway, signal fields = 0)\r\n");
    }

    p  = body;
    p += sprintf(p, "#%lu,CSQ=%d,RSRP=", (unsigned long)seq, r.csq);
    p += put_tenth(p, r.rsrp);
    p += sprintf(p, ",SNR=");
    p += put_tenth(p, r.snr);
    p += sprintf(p, ",ECL=%d,UP=%lu", r.ecl,
                 (unsigned long)(BC28_Millis() / 1000));

    if (dht_rc == 0)
    {
        dht_temp_str(ts, &dht);
        p += sprintf(p, ",T=%s", ts);
    }
    else
    {
        p += sprintf(p, ",T=-99.0,DHTE=%d", dht_rc);
    }

    /* 回显板子**当前**在跑的档位。服务端那份配置跟板子实际在跑的可能不一致
       —— 看板点了开关, 要等板子下一次上报才取回去, 中间隔着一个上报周期。
       看板把这一行跟服务端的值并排显示, 配错了当场看得见, 不用去猜
       "到底生效了没有"。
       字节预算: body[128]。**最坏情况逐段数过 = 119**(seq/UP 十位、T 那栏走
       DHTE 分支、RM=3600000、再加满位的 CID=268435455,PCI=503), 含 \r\n 剩 9 字节。
       数得这么细是因为这里全是 sprintf 进定长缓冲, 没有 snprintf 兜底 ——
       以后再加字段, 先把这段重数一遍。 */
    p += sprintf(p, ",RM=%lu,S=%d,P=%d",
                 (unsigned long)report_ms, cfg_stop, cfg_psm);

    /* ---- 当前小区 ----
       只在**真读上来**的时候才发(见 bc28.h 里 BC28_RADIO 那段: 读不到是 -1)。
       读不到就整段不发, 服务端和看板只要判"报文里有没有 CID="就行 —— 比发个
       CID=0 再去猜"0 是真小区号还是没读到"干净得多。

       服务端那边 CID/PCI 存成 NULL、看板显示"本条没带 CID=", 两种老情况
       (固件没升 / 这轮 NUESTATS 没读上来)长得一样, 也都是对的。 */
    /* ★ 取值范围**必须卡死**, 两个理由, 第二个是硬的:
         1) 合法性: Cell ID 是 28 位、PCI 是 0~503, 超出这个范围的就是模块
            回上来的垃圾, 发出去只会让看板显示一个假小区号。
         2) **字节预算**: nstats_val 底层是 int, 真跑飞了能回 10 位数甚至负数
            (`CID=-2147483647,PCI=-2147483647` = 31 字节)。不卡的话 128 的缓冲
            会被撑爆 —— 这里全是 sprintf, 没有 snprintf 兜底, 溢出就是踩内存。
         卡住之后 CID 最多 9 位、PCI 最多 3 位, 全句最坏 119 字节, 见上面那段注释。 */
    if ((r.cell_id >= 0) && (r.cell_id <= 268435455) &&
        (r.pci     >= 0) && (r.pci     <= 503))
    {
        p += sprintf(p, ",CID=%d,PCI=%d", r.cell_id, r.pci);
    }

    p += sprintf(p, "\r\n");            /* 行尾换行, 理由见 link_up() 里那段注释 */

    rc = BC28_Send(sock, body, (uint16_t)strlen(body));
    if (rc < 0)
    {
        printf("[%lu] send failed: %s\r\n", (unsigned long)seq, BC28_Why(rc));
        BC28_DumpRx();
        rx_lost++;
        return;
    }

    /* ---- 读应答 ----
       **要跳过迟到的 "AUTH OK"** —— 那是握手那一行的回执。link_up() 等它只等
       2 轮, 等不到就放过去了(不作为失败), 于是它常常在下面这一两轮才浮上来。
       不跳过的话它会被当成"这一条上报的 ACK", 于是 lost 计数**虚高**: 实测里
       串口报 lost 3, 而服务端日志显示那 3 条一条不少全到了 —— 是判据错, 不是
       链路错。这条记在第 3 篇 §8。

       ⚠ 判据已经从 `strcmp(buf, "AUTH OK")` 换成了 ack_is_data() ——
         因为服务端现在回的是 "AUTH OK\n#CFG ...", 那个 strcmp **永远不成立**,
         这颗雷就哑了(而且哑得很安静: 它只是让计数慢慢偏掉, 不报错)。
         新旧判据怎么分得开, 写在 ack_is_data() 上面那段。

       每一轮回执都顺手 cfg_scan 一下, **不管它是不是这一轮的 ACK** ——
       服务端每条都带配置, 哪条上都可能有新的。

       got_ack 而不是"n > 0": 三轮都只收到迟到的 AUTH OK 的话, n 是 > 0 的,
       但这一轮的 ACK 确实没来 —— 那种情况该记 lost, 不该记 OK。 */
    got_ack = 0;
    for (i = 0; i < 3; i++)
    {
        n = wait_reply(sock, buf, sizeof(buf), 3);
        if (n <= 0)
        {
            break;                      /* 这几轮一个字节都没有, 不用再等 */
        }

        (void)cfg_scan(buf);

        if (ack_is_data(buf))
        {
            got_ack = 1;
            break;                      /* 就是它了 */
        }

        printf("  (late) server: %s -- not this round's ACK, keep waiting\r\n", buf);
    }

    if (got_ack)
    {
        rx_ok++;
        printf("[%lu] %s  <- %s   (server OK %lu, lost %lu)\r\n",
               (unsigned long)seq, body, buf,
               (unsigned long)rx_ok, (unsigned long)rx_lost);
    }
    else
    {
        rx_lost++;
        printf("[%lu] %s  <- no reply   (server OK %lu, lost %lu)\r\n",
               (unsigned long)seq, body,
               (unsigned long)rx_ok, (unsigned long)rx_lost);
    }
}

/**
  * @brief  Main program.
  */
int main(void)
{
    char     info[128];
    uint32_t cal;
    int      rc;
    int      i;

    /* 1ms SysTick 中断(TimingDelay_Decrement 由它调, 不能删) */
    if (SysTick_Config(SystemCoreClock / 1000))
    {
        while (1);
    }

    LED_Init();
    uart1_init(9600);   /* USART1 -> CH340 -> 电脑: printf 就是走这条路出去的。
                           漏了这一句的现象很典型: 烧录成功、板子在跑, 但串口
                           一个字都没有 —— 因为 USART1 的时钟和引脚都还没配。 */
    BC28_Init();        /* 里面会 uart2_init(9600) */
    DHT11_Init();       /* PB8 开漏 + TIM6 微秒时基。放在 uart1_init 之后,
                           这样它要是哪天会打印, 日志也出得来 */

    printf("\r\n\r\n");
    printf("### Part 3: uplink to my own server ###\r\n");
    printf("build:  " __DATE__ " " __TIME__ "\r\n");
    printf("server: %s:%u   mode: %s   report: %lu ms\r\n",
           SERVER_HOST, (unsigned)SERVER_PORT, MODE_NAME, (unsigned long)report_ms);
    /* 下行配置的当前值。开机时当然是出厂默认 —— 真正的档位要等第一次
       握手/上报才从服务端取回来, 那时会打一行 "+ CFG from server"。 */
    printf("config: report=%lu ms  stop=%d  psm=%d   (runtime only, not in flash;"
           " power cycle -> %lu/0/0)\r\n",
           (unsigned long)report_ms, cfg_stop, cfg_psm,
           (unsigned long)REPORT_MS_DFLT);
    /* 这句错不起 —— 它会直接把后面查问题的人带沟里。
       stop 这一批**已经真执行了**(就是进 STOP); psm 还只是收下+回显,
       那是最后一批的事。RTC 唤醒源的状态在 BC28_Init() 里已经打过一行。 */
    printf("      : stop -> real STOP mode (%s); psm -> received+echoed only\r\n",
           (BC28_RtcOk() != 0) ? "RTC wakeup armed"
                               : "** DISABLED: no RTC wakeup source **");
    printf("target is %s -- %s\r\n",
           is_ipv4(SERVER_HOST) ? "an IP" : "a hostname",
           is_ipv4(SERVER_HOST) ? "connecting directly"
                                : "will resolve with AT+QDNS on every connect");
#if (BC28_SKIP_NSOCO_URC != 0)
    printf("!! A/B build BC28_SKIP_NSOCO_URC=1: send right after OK,");
    printf(" do NOT wait for +NSOCO URC\r\n");
#endif
    printf("\r\n");

    /* ---- ⓪ 时基自检 ----
       Delay(1000) 是 SysTick 中断驱动的(第 4 篇拿它量过墨水屏刷新, 数值对得上
       数据手册), 所以它可以当尺子。拿它量 TIM4 那套毫秒时基 ——
       这一句能一眼抓住"尺子错了"那类 bug。第一版我就是栽在这儿: TIM4 预分频
       写成了 1MHz(每格 1 微秒), 而代码把每格当 1 毫秒, 于是"等 1 秒"实际只等
       了 1 毫秒, 模块永远来不及回话 —— 日志上是"AT 打了 9000 次一个字节没有",
       看着像模块死了, 其实是量时间的尺子快了 1000 倍。 */
    {
        cal = BC28_Millis();
        Delay(1000);
        cal = (uint32_t)(BC28_Millis() - cal);
        printf("+ timebase self-check: Delay(1000) -> %lu ms  %s\r\n",
               (unsigned long)cal,
               ((cal > 900) && (cal < 1100)) ? "OK" : "** WRONG **");
        if ((cal <= 900) || (cal >= 1100))
        {
            printf("! the ms timebase is off -- every timeout below is scaled by"
                   " the same factor, fix tim4_init() before believing anything\r\n");
        }
    }

#if SLEEP_SELFTEST
    /* ---- ⓪.2 STOP 唤醒自检 ----
       跑 50 次 5 秒 = 4 分 10 秒 + 唤醒开销, 一次烧录就能把唤醒这条路走 50 遍。

       **每一轮都重新量一次 Delay(1000)**, 这是整个自检的核心:
         Delay() 用的是 SysTick 中断(TimingDelay), 而 SysTick 在睡前被显式关掉了
         (不关根本进不去 STOP), 醒来必须重新 SysTick_Config。它同时还能证明
         32MHz 重建对了 —— SysTick 的装载值是按 SystemCoreClock 算的,
         时钟没拉回 32MHz 的话 Delay(1000) 就不是 1000。

       三种结果分开看, 别混:
         slept~5000 且 Delay(1000)->1000   = 全对
         slept~0    或 rtc=0               = 没进 STOP(RTC 那一侧的问题)
         Delay 不是 1000                   = 时钟/时基那一侧的问题

       ⚠ 这里是**有限次**循环, 不能写 for(;;) —— 后面还有代码, 写成死循环
         AC5 会报 #111-D 不可达代码警告, 而本工程要求 0 Warning。 */
    {
        uint32_t k;
        uint32_t t1;
        uint32_t d;

        printf("--- SLEEP_SELFTEST: 5 s x 50 (rtc=%u: 0=none 1=LSE 2=LSI) ---\r\n",
               (unsigned)BC28_RtcOk());

        for (k = 0; k < 50UL; k++)
        {
            t1 = BC28_Millis();
            (void)BC28_SleepMs(5000UL);
            t1 = (uint32_t)(BC28_Millis() - t1);

            d = BC28_Millis();
            Delay(1000);
            d = (uint32_t)(BC28_Millis() - d);

            printf("  [%02lu] slept %5lu ms   Delay(1000) -> %lu ms  %s"
                   "   up=%lu s\r\n",
                   (unsigned long)k, (unsigned long)t1, (unsigned long)d,
                   ((d > 900UL) && (d < 1100UL)) ? "OK  " : "** WRONG **",
                   (unsigned long)(BC28_Millis() / 1000UL));
        }

        printf("--- SLEEP_SELFTEST done: stops=%lu  slept=%lu s total  rtc=%u ---\r\n",
               (unsigned long)BC28_SleepCnt(),
               (unsigned long)(BC28_SleepMsSum() / 1000UL),
               (unsigned)BC28_RtcOk());
        printf("--- 'slept' should be ~5000 every round; a 0 means it did NOT"
               " enter STOP ---\r\n\r\n");
    }
#endif

#if DHT11_SELFTEST
    /* ---- ⓪.5 温湿度自检 ----
       放在模块探测**之前**: 能不能读 DHT11 跟 NB-IoT 链路毫无关系,
       先把它调通, 后面万一出问题就不用怀疑这一侧。

       跑 10 次、每次隔 2 秒(DHT11 的采样间隔下限) = 20 秒, 正好一次
       read_com.ps1 -Seconds 25 的量 —— **一次烧录换 20 个数据点**,
       调硬件的时候这个很省事, 不然就是改一次烧一次。

       ⚠ 用**有限次**循环而不是 for(;;): 后面还有 BC28_Idle 那一句,
         写成 for(;;) 会让它变成不可达代码, AC5 报 #111-D 警告,
         而本工程要求 0 Warning。

       两行输出里的第二行(实测量)才是这一节的重点 —— 见 dht11.h 里那段:
       DHT11 的 0 和 1 只差在高电平宽度, 把 hi0<= / hi1>= 打出来就知道
       门槛 50us 两边各有多少余量。接错线的时候也是靠这几个数一眼看出来
       "时序根本不对", 而不是傻猜"传感器是不是坏了"。

       ---- 手上这颗传感器实测出来的两件事(第 5 篇要写进去) ----

       1) **它的温度是 0.1 分辨率, 所以它不是纯 DHT11。** 真 DHT11 的温度
          分辨率是 1 摄氏度, 小数那两个字节恒为 0。这颗给的 raw 是
          `1A 09` -> `1B 00` -> `1B 01` ... 也就是 26.9 -> 27.0 -> 27.1,
          中间那一步是**进位**(小数从 9 滚到 0, 整数 +1)。协议一样但分辨率
          不一样, 说明它是 DHT12 或同协议的克隆片。**驱动不用改** ——
          dht11.c 本来就把 4 个字节都收全了, 小数位照收(from day one)。

       2) **它的湿度通道恒为 0, 但校验和是对的。** raw 的前两个字节每次都是
          `00 00`, 而第 5 个字节仍然等于前 4 个之和 —— 40 位里错一位还能撞对
          校验和的概率是 1/256, 连中 10 次不可能。所以结论只能是一个:
          **不是读错了, 是它真在发 0**。房间里不可能有 0%RH(传感器量程下限
          本身就是 20%), 所以这一栏是坏的, 上报里干脆不发 `,H=`。
          —— 这一条本身就是个挺好的教训: **校验和通过 != 数据有意义**,
             dht11.c 里那道 E_RANGE 量程检查就是为这类"看着合法其实全错"
             准备的(只是它拦的是超量程, 拦不住"合法的 0")。 */
    {
        char ts[16];
        int  k;

        printf("\r\n--- DHT11 self-test: 10 reads, 2 s apart ---\r\n");
        for (k = 1; k <= 10; k++)
        {
            dht_rc = DHT11_Read(&dht);
            dht_ms = BC28_Millis();

            if (dht_rc == 0)
            {
                dht_temp_str(ts, &dht);
                printf("+ [%d/10] T=%sC   raw=%02X %02X %02X %02X %02X\r\n",
                       k, ts,
                       (unsigned)dht.raw[0], (unsigned)dht.raw[1],
                       (unsigned)dht.raw[2], (unsigned)dht.raw[3],
                       (unsigned)dht.raw[4]);
            }
            else
            {
                printf("- [%d/10] FAIL (%d): %s\r\n", k, dht_rc, DHT11_Why(dht_rc));
                if (dht_rc == DHT11_E_NORESP)
                {
                    printf("          check: sensor powered? data wire on B8");
                    printf(" (the middle pin)? module pin order right?\r\n");
                }
                else if (dht_rc == DHT11_E_STUCK)
                {
                    printf("          check: line shorted to GND? module's own");
                    printf(" pull-up powered? (this board has none)\r\n");
                }
            }

            /* 这一行**成功失败都打** —— 失败时它才是唯一线索。 */
            printf("          start=%u resp=%u/%u hi0<=%u hi1>=%u lo<=%u frame=%u\r\n",
                   (unsigned)dht.start_us, (unsigned)dht.resp_low,
                   (unsigned)dht.resp_high, (unsigned)dht.hi0_max,
                   (unsigned)dht.hi1_min, (unsigned)dht.lo_max,
                   (unsigned)dht.frame_us);

            if (k < 10)
            {
                BC28_Idle(DHT11_MIN_PERIOD_MS);     /* 一边等一边收 URC */
            }
        }
        printf("--- DHT11 self-test done ---\r\n\r\n");
    }
#endif

    /* ---- ① 模块在不在 ----
       别指望第一枪就中: 模块平时在 PSM/eDRX 里睡着(第 2 篇查过, 出厂就开着),
       睡着的模块被 UART 上第一个字符叫醒, 而**那个字符自己常常是丢的**。
       厂家 BC28_Init() 也是拿 while 死磕 ATE1 直到回 OK, 只是它没说为什么。
       这里连打不停, 并把"第几次才应答"打出来 —— 这个次数本身就有意义。 */
    BC28_Idle(1500);            /* 上电先给它一点时间, 别一上来就砸 */

    for (i = 1; ; i++)
    {
        info[0] = '\0';
        rc = BC28_Probe(info, sizeof(info));
        if (rc == 0)
        {
            printf("+ modem answered on AT attempt %d: %s\r\n", i, info);
            break;
        }
        if ((i == 1) || ((i % 5) == 0))
        {
            printf("- AT attempt %d: %s\r\n", i, BC28_Why(rc));
            if (i <= 2)
            {
                BC28_DumpRx();  /* 一个字节都没有 = 模块根本没在说 */
            }
        }
        if (i >= 60)            /* 60 次 x 1 秒还不应, 那就是真不通, 报一次再接着试 */
        {
            printf("! no answer after 60 AT attempts\r\n");
            printf("! check: SW8 switch / BC28 module power / PA2-PA3\r\n");
            i = 0;
        }
        BC28_Idle(500);
    }

    /* ---- ② 注网 ---- */
    /* 2026-09-15 改: 这里以前是 `while(1) { BC28_Idle(500); }` —— 开机注网一旦超时,
       就永久静默死等, 串口不出声、看板没数据、AT 也不回, **看起来跟板子坏了一模一样**,
       而且断电重开还是这样(开机 BC28_Init 会 AT+NRB 重启模块, 每次都得重新注网)。
       实测代价: 用户以为板子坏了, 查了一晚上。
       现在改成一直重试: 超时就把模块 AT+NRB 重启再来一轮。
       板子宁可慢一点起来, 也绝不出这种"安静地死掉"的状态。
       注: 主循环里早就有同一套重试(connect_failed -> hard_recover), 只有启动这条路漏了。

       ★ 2026-09-16 又改一次 —— 原来这里**无脑 AT+NRB**, 结果反而把板子卡死了:
       AT+NRB 之后模块要从头搜网**约 2 分钟**才附着, 而 BC28_WaitNet 当时的超时是
       120 秒(正好卡在边界上), 超时一到就重启模块 → 刚搜到一半被打断 → 再搜 →
       再超时 …… **永远搜不完**。串口指纹: 一直 `CEREG stat=2`(搜网中) + `CSQ 25~26`
       (信号好得很) + 每 120 秒一条 `! network attach timeout` 跟一条 `! rebooting BC28`。
       现在只在模块**完全不吭声**(BC28_E_NORESP)时才 NRB; 有回话就继续等。
       见 bc28.h 里 BC28_TO_NET 与 bc28.c 里 BC28_WaitNet 的注释。 */
    {
        uint32_t round = 0;

        for (;;)
        {
            round++;
            printf("+ waiting for network (AT+CEREG?) ... round %lu\r\n",
                   (unsigned long)round);

            rc = BC28_WaitNet(BC28_TO_NET);
            if (rc == 0)
            {
                break;
            }

            if (rc == BC28_E_NORESP)
            {
                /* 模块一个字都不回 = 真卡死, 这才是 NRB 该上的时候 */
                printf("! modem silent (%s) -- rebooting BC28 (AT+NRB) ...\r\n",
                       BC28_Why(rc));
                BC28_Reset();
            }
            else
            {
                /* 模块有回话, 只是在搜小区 —— **别打扰它**。
                   这里原来是无脑 NRB, 正是它把附着一次次打断的。 */
                printf("! still searching (%s) -- keeping the attach alive\r\n",
                       BC28_Why(rc));
            }
            BC28_Idle(1000);
        }
    }
    printf("+ attached (CEREG = 1 or 5)\r\n");

    /* ---- ③ 记一次信号, 让你一眼看到天线和卡是好的 ---- */
    {
        BC28_RADIO r0;
        char       rb[64];
        char      *q = rb;

        if (BC28_GetRadio(&r0) == 0)
        {
            q += sprintf(q, "CSQ=%d  RSRP=", r0.csq);
            q += put_tenth(q, r0.rsrp);
            q += sprintf(q, " dBm  SNR=");
            q += put_tenth(q, r0.snr);
            q += sprintf(q, " dB  ECL=%d  band=%d", r0.ecl, r0.band);
            printf("+ radio: %s\r\n", rb);
        }
        else
        {
            printf("! could not read radio (going on anyway)\r\n");
        }
    }

    /* ---- ④ 主循环 ---- */
    printf("\r\n--- uplink started ---\r\n");

    for (;;)
    {
        uint32_t t0 = BC28_Millis();

        seq++;

        /* ---- ④-0 socket 账本自检 + 保险丝 ---- */
        {
            int bal = BC28_SockBalance();

            /* 每一轮都打。泄漏是"一轮漏一点"攒出来的, 只在出事时才打就**看不到
               它是从哪一轮开始斜的** —— 而那个拐点才是查根因的入口。
               一行很短, 30 秒档每分钟两条、300 秒档每 5 分钟一条, 不心疼。

               ⚠ balance 只是**参考值**, 不再当保险丝的判据 —— 理由见下面。 */
            printf("  [sock] open=%lu close=%lu balance=%d  nsocli=%d/%lu\r\n",
                   (unsigned long)BC28_SockOpenCnt(),
                   (unsigned long)BC28_SockClosedCnt(), bal,
                   BC28_SockCliLast(), (unsigned long)BC28_SockCliCnt());

            /* ★★ 保险丝 (2026-09-18 晚重写: 判据从**余额**换成**开不出号**)

               原来判的是 `bal >= 4`。**2026-09-18 23:14 实测它是误报**:
               那一轮 open=10 close=6 balance=4, 保险丝 NRB, 而 NRB 之后每一轮
               都是 `open=N close=N balance=0` —— 模块压根没漏, 是**账本记歪了**。

               记歪的根子: `AT+NSOCL` 回 modem-ERROR 有**两种**意思, 而账本只
               当成一种:
                 (a) 关不掉, 号还占着        -> 真泄漏
                 (b) 这个号本来就已经没了     -> **号早就还回去了**
                `+NSOCLI` 一到就说明是 (b): 网络刚通知"n 号我关了", 模块内部已经
               放掉, 我们紧接着去关一个不存在的号, 当然回 ERROR。
               (bc28.c 里 BC28_Close 自己的注释早写了"已经关掉的 socket 再关一次
                模块回 ERROR, 无害" —— 那句是对的, 是账本把"无害"记成了"漏一个"。
                23:12:49 那行 `[sock] ... balance=1 nsocli=3` 就是现场: 先收到
                +NSOCLI: 3, 紧接着三次 NSOCL=3 全回 modem-ERROR。)
               代价是每次误触发 **NRB = 2~8 分钟搜网 = 看板上一条离线**。

               真正的漏光指纹只有一个, 而且 BC28_Open() 早就把它分出来了:
                 **注着网, 却连着几次一个号都开不出来**(NSOCR 回 ERROR / 回 OK 没号)。

               ⚠ 没注网的时候 NSOCR 也回 ERROR, 所以**必须先问一句注网没有**,
                 否则又是同一个误报 —— 这跟 connect_failed() 里那个"没注网就别
                 NRB"是同一条教训, 那边已经踩过一次了。 */
            if (nosock_run >= 3)
            {
                if (BC28_WaitNet(0) == 0)
                {
                    printf("! NSOCR failed %d times in a row while ATTACHED --\r\n",
                           nosock_run);
                    printf("! sockets really are gone -- rebooting the module\r\n");
                    nosock_run = 0;
                    sock = -1;          /* 模块要重启了, 手里这个号马上就不作数 */
                    (void)hard_recover();   /* 里面会 BC28_Reset(), 账本跟着清零 */
                    connect_failed(3000, BC28_E_NOSOCK);
                    continue;
                }

                printf("! NSOCR failed %d times, but the module is NOT attached --\r\n",
                       nosock_run);
                printf("! that is not a leak (NRB would eat the search), waiting\r\n");
                nosock_run = 0;
            }
        }

#if MODE_LONG
        /* 运营商经常把闲着的 socket 掐掉, 掐的时候模块会给一条 +NSOCLI。
           驱动的每次收发都顺手扫它, 这里检查一下就够了 —— 不用另外去 ping。 */
        if (BC28_SocketClosed())
        {
            printf("! got +NSOCLI: socket closed by network, reconnecting\r\n");
            (void)BC28_Close(sock);
            sock = -1;
        }

        if (sock < 0)
        {
            sock = link_up();
            if (sock < 0)
            {
                /* 只有"注着网却开不出号"才记进 nosock_run (见 ④-0 那段) */
                nosock_run = (sock == BC28_E_NOSOCK) ? (nosock_run + 1) : 0;
                connect_failed(5000, sock);
                continue;
            }
            conn_fail = 0;
            nosock_run = 0;
            GPIO_SetBits(GPIOC, GPIO_Pin_3);        /* 板上那颗灯 = 连着 */
        }

        report_once();
#else
        /* 短连接: 每轮 建连 -> 发 -> 收 -> 关, 专门用来量建连耗时 */
        {
            uint32_t dt;
            int      s2 = link_up();

            if (s2 < 0)
            {
                /* 只有"注着网却开不出号"才记进 nosock_run (见 ④-0 那段) */
                nosock_run = (s2 == BC28_E_NOSOCK) ? (nosock_run + 1) : 0;
                connect_failed(3000, s2);
                continue;
            }
            conn_fail = 0;
            nosock_run = 0;
            sock = s2;
            GPIO_SetBits(GPIOC, GPIO_Pin_3);

            dt = BC28_Millis() - t0;
            conn_n++;
            conn_sum += dt;
            if ((conn_n == 1) || (dt < conn_min)) { conn_min = dt; }
            if (dt > conn_max) { conn_max = dt; }
            printf("  [connect #%lu] %lu ms   (min %lu / max %lu / avg %lu, n=%lu)\r\n",
                   (unsigned long)conn_n, (unsigned long)dt,
                   (unsigned long)conn_min, (unsigned long)conn_max,
                   (unsigned long)(conn_sum / conn_n), (unsigned long)conn_n);

            report_once();

            (void)BC28_Close(sock);
            sock = -1;
            GPIO_ResetBits(GPIOC, GPIO_Pin_3);
        }
#endif

        /* 等下一轮 —— **整轮里唯一该进 STOP 的地方**。
           前面在建连/发/收, 后面马上要上报, 中间这段(30 秒或 300 秒)才是大头。
           建连和收数的间隙_不要_睡: 那时候正跟模块对话, 睡下去必丢字节。

           ⚠ 用 report_ms 而不是 REPORT_MS_DFLT: 这个值是**上一轮**上报时从
             服务端取回来的。看板改了档位、板子这边要是正好卡在等待里, 那它
             会一直等到这一轮结束才看到新值 —— 这正是"下次上报时顺便取回"
             的意思, 不是 bug。周期 300 秒时最长等 5 分钟才切回来。

           BC28_SleepMs() 自己会判断能不能睡(rtc_ok、以及剩余时间够不够长),
           睡不了就退回空转, 返回值也已经补进 BC28_Millis —— 所以这个循环
           的判据不用为 STOP 改一个字。 */
        {
            uint32_t sl0 = BC28_SleepCnt();
            uint32_t tw;

            /* tw 单量"等待"这一段, 不能拿 t0 充数 —— t0 是整轮开头,
               BC28_Millis() - t0 量的是整轮(含建连和收发), 打出来对不上。 */
            tw = BC28_Millis();

            while ((uint32_t)(BC28_Millis() - t0) < report_ms)
            {
                uint32_t left = report_ms - (uint32_t)(BC28_Millis() - t0);

                /* ★ cfg_stop 必须**真的**闸住这里。
                   2026-09-16 发现它以前只被存下、回显、打印, 从头到尾没有 if 过 ——
                   于是"正常档"照样进 STOP: 看板显示"正常"、板子也回显 S=0,
                   而串口那行 `-- wait 9000 ms, asked 30000, STOP x1` 照样出现。
                   后果不只是省电没关掉: **STOP 会把 SWD 调试口关掉, 所以"正常档"
                   下烧录照样失败**(见 memory「省电档下烧不进去」)。
                   正常档的定义就是**一直醒着**, 这里不修那个档位就是假的。 */
                if (cfg_stop != 0)
                {
                    (void)BC28_SleepMs(left);
                }
                else
                {
                    BC28_Idle(left);
                }
            }

            /* 这一行是"到底有没有真进 STOP"的**直接证据**。
               ⚠ 光看上报间隔看不出来 —— 空转也是等这么久。而"电流没降"的原因
                 有好几个(见验证 11: ST-Link 还插着的话 DBGMCU 的 DBG_STOP
                 会让 MCU 根本不进 STOP)。所以要有这一行, 别去猜。 */
            printf("-- wait %lu ms, asked %lu, STOP x%lu (rtc=%u, total %lu)\r\n",
                   (unsigned long)(uint32_t)(BC28_Millis() - tw),
                   (unsigned long)report_ms,
                   (unsigned long)(BC28_SleepCnt() - sl0),
                   (unsigned)BC28_RtcOk(),
                   (unsigned long)BC28_SleepCnt());
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
