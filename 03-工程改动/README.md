# 工程改动的 diff（`Project.uvproj` / `Project.uvopt`）

这里就两份 diff，记录**本工程相对厂家原件改了哪几行**。

| 文件 | 内容 |
|---|---|
| [`Project.uvproj.diff`](Project.uvproj.diff) | 厂家原件 → 已修，5 处改动（编译期 + target 1 的烧录驱动串） |
| [`Project.uvopt.diff`](Project.uvopt.diff) | 厂家原件 → 已修，1 处改动（调试器那条驱动串） |

---

## 为什么发 diff，而不是把整份工程发上来

1. 整份 `Project.uvproj` 是 **12 万字节的 XML**，其中 99% 是厂家工程自己的配置，跟"改了什么"无关；
   直接贴出来，真正改的那 5 行会被淹掉。
2. 那是**厂家资料包里的文件**，本仓库不重新分发厂家资料。要原件去资料包里拿，
   这里只告诉你**拿到之后要改哪几行**。

所以：**这两份 diff 是"说明书"，不是"补丁文件"**（没法 `patch` 到别的例程上 —— 原因见
下面的「别照抄」）。

---

## 三处缺陷

资料包里 13 个例程工程，**每一个的 target 1 都带同样三处缺陷**
（target 1 = 本板芯片 `STM32L1XX_MD(STM32L1xxxBxx)` / `STM32L151RC`；target 2/3/4 是别的芯片，不用管）：

| # | 缺陷 | 不修的后果 |
|---|---|---|
| ① | `IncludePath` 缺 `..\..\..\Libraries\CMSIS\Include` | 报 `cannot open source input file "core_cm3.h"`，**编不过** |
| ② | 烧录驱动串缺 `-FP0(<FLM 绝对路径>)` | 烧写算法没加载，报 **`Erase Failed!`**（这个错误信息**极具误导性**，跟读保护/容量都没关系），**烧不进去** |
| ③ | 烧录驱动串是 `-FO7` 而不是 `-FO15` | `-FO15` 才是 Keil 那个「**Reset and Run**」。`-FO7` 会**下载成功、`Verify OK`，但芯片被留在停机状态，串口一个字都没有** |

三处各对应一个"看起来完全不相干"的现象，所以很容易在三个坑里各摔一次。

---

## 这两份 diff 分别管什么

| 文件 | 改的是什么 | 对应缺陷 |
|---|---|---|
| `Project.uvproj.diff` | 4 个 target 的 `<IncludePath>` 尾巴上补 `..\..\..\Libraries\CMSIS\Include`；target 1 的 `<FlashDriverDll>` 换掉 `-FO7`、补上 `-FP0(...)` | ① ② ③ |
| `Project.uvopt.diff` | `<Key>ST-LINKIII-KEIL_SWO</Key>` 那条的 `<Name>` 里，同样换 `-FO7`→`-FO15`、补 `-FP0(...)` | ② ③ |

⚠️ **为什么同一件事要改两个文件**：这份烧录参数**在工程里存了两处** ——
`.uvproj` 的 `<FlashDriverDll>` 和 `.uvopt` 的 `<Name>`。**Keil 下载时读的是 `.uvopt` 那条**
（`.uvproj` 那份更像个"默认值模板"）。只改 `.uvproj`，Keil 照样用 `.uvopt` 里的旧参数去下载，
症状一模一样 —— 这是当初最费时间的一个坑。

也因此：**谁在 Keil 图形界面里动过** Options for Target → Utilities → Settings → Flash Download
（加过/删过算法、改过 Reset and Run），**Keil 会把 `.uvopt` 那条整个重写**，② ③ 就可能回来，
得重跑一次修复（手册 §6.9）。

---

## 怎么读这两份 diff

- 文件头几行是说明；`---` / `+++` 之后才是 diff 本体。
- **这不是完整 diff**：Keil 5 打开并**保存**这个 2014 年的老工程时，会把它自己的一整套默认选项
  （`<uC99>` `<v6WtE>` `<ComprImg>` `<useXO>` … 约 180 行）补写进去。那些 hunk 与本次修复无关，
  **已整块滤掉**（滤掉 76 个 hunk，全是这类），否则真正的改动会被埋掉。
- 每个留下的 hunk 里**还夹着几行这种默认项**（Keil 是往 `<uSurpInc>` 和 `<VariousControls>` 之间
  插的标签，`<IncludePath>` 就在 `<VariousControls>` 下一层，所以两者在 diff 里只隔一行，滤不干净）。
  **那些不是修复内容** —— 认 **`-`/`+` 成对出现、且带 `<IncludePath>` / `<FlashDriverDll>`** 的那两行。
- 行号**仍是真实文件里的行号**，可以照着在 Keil 界面里对着改。

真正改动的地方，短这样：

```diff
-<FlashDriverDll>ULP2CM3(-O207 -S8 -C0 -FO7  -FD20000000 -FC800 -FN1 -FF0STM32L1xx_256 -FS08000000 -FL040000)</FlashDriverDll>
+<FlashDriverDll>ULP2CM3(-O207 -S8 -C0 -FO15 -FD20000000 -FC800 -FN1 -FF0STM32L1xx_256 -FS08000000 -FL040000 -FP0(C:\Keil_v5\ARM\PACK\Keil\STM32L1xx_DFP\1.2.0\Flash\STM32L1xx_256.FLM))</FlashDriverDll>
```

`IncludePath` 那 4 处，就是**在原有列表尾巴上加一段**：

```diff
-...\HARDWARE\LED;..\..\HARDWARE\eink</IncludePath>
+...\HARDWARE\LED;..\..\HARDWARE\eink;..\..\..\Libraries\CMSIS\Include</IncludePath>
```

---

## 想在 Keil 界面里手改

| 缺陷 | 点哪里 |
|---|---|
| ① | 魔术棒（Options for Target）→ **C/C++** → **Include Paths** → 末尾加一条 `..\..\..\Libraries\CMSIS\Include` |
| ② ③ | 魔术棒 → **Utilities** → **Settings** → **Flash Download** → 确认算法列表里有 `STM32L1xx_256`（②），并勾上 **Reset and Run**（这就是 ③ 的 `-FO15`） |

改完**关掉 Keil** 再跑下载（界面开着会占着 ST-Link，见手册 §6.4）。

---

## ⚠️ 别照抄这两份 diff

- **③ 每个例程取值不一样**：例程 3 是 `-FO7`（要改），**例程 12 本来就是 `-FO15`**（不用改）。
- **① 的 `IncludePath` 也因例程而异**：本工程那份尾巴上有 `HARDWARE\LED`、`HARDWARE\eink`，
  别的例程没有这些目录。
- **② 的 `-FP0(...)` 里是绝对路径**，写的是**作者这台机器**上 Keil 器件包的安装位置。
  你机器上 Keil 装到别处、或器件包版本不同（不是 `1.2.0`），这个路径就不对 —— 得换成你自己的。

**正确做法**：别手抄，跑工具让它按**你这个例程的现状**判断：

```bat
python "<工具目录>\prep_project.py" <例程的 MDK-ARM 目录>
```

它是幂等的（已经对的不动）、自动留 `.bak-prep` 备份，逐 target 检查，
先 `--check` 可以只看不改（见仓库 `02-工具\`）。判据：烧录日志里有没有
**`Application running ...`** 这句。

---

## 这两份 diff 是怎么来的

| | 原始件（厂家） | 已修件（本工程在用） |
|---|---|---|
| `Project.uvproj` | 120560 字节，2018-11-27，md5 `1b25b41ec29e80cb2635d81171cac77c` | 127303 字节 |
| `Project.uvopt` | 44043 字节 | 44119 字节 |

生成方式：对两个文件各做一次 `diff -u 原始 已修`，再把 76 个"纯 Keil 默认项"hunk 整块滤掉，
头几行的说明是手写的。原始件与已修件都留在本机（不发布 —— 含厂家工程内容）。
