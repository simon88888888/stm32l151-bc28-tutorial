#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
prep_project.py —— 把厂家的 STM32L1xx 例程工程改成「能编译 + 能烧录 + 烧完会跑」。

用法:
    python prep_project.py <MDK-ARM 目录或 Project.uvproj 路径> [--check] [--flm <路径>]

换台电脑也能用：Keil 安装位置和 .FLM 烧写算法都是**自动探测**的（注册表 +
常见盘符），不写死本机路径；探测不到时用 --flm 手工指定。

为什么需要这个脚本：资料包里 13 个例程工程，**每一个的 target 1 都带着同样三处缺陷**
（target 1 = 本板芯片 STM32L1XX_MD(STM32L1xxxBxx) / STM32L151RC，2/3/4 是别的芯片不用管）：

  ① IncludePath 少 `..\\..\\..\\Libraries\\CMSIS\\Include`
       -> 报 cannot open source input file "core_cm3.h"，**编不过**
  ② ST-Link 驱动串少 `-FP0(<FLM 绝对路径>)`
       -> 算法没加载，报 `Erase Failed!`，**烧不进去**（错误信息极具误导性，跟保护/容量无关）
  ③ ST-Link 驱动串是 `-FO7` 而不是 `-FO15`
       -> `-FO15` 才是 Keil 的「Reset and Run」，`-FO7` 会**烧进去但芯片被留在停机状态，
          串口一个字都没有**。注意这一项**每个例程不一样**（例程 3 是 7、例程 12 是 15），
          所以必须逐个检查，不能照抄。

三处都改完，才能用 flash_and_run.ps1 编译+烧录+看日志。
"""

import os
import re
import shutil
import sys
import glob

try:
    # 默认按控制台编码（中文 Windows = GBK，在 cmd/PowerShell 里显示正常）。
    # 这里只是保证遇到编码不了的字符也不会崩；Git Bash(UTF-8) 下想看清就加
    #   PYTHONIOENCODING=utf-8
    sys.stdout.reconfigure(errors='replace')
except Exception:
    pass

FLM_FALLBACK = r'C:\Keil_v5\ARM\PACK\Keil\STM32L1xx_DFP\1.2.0\Flash\STM32L1xx_256.FLM'
CMSIS_REL = r'..\..\..\Libraries\CMSIS\Include'
FLM_NAME = 'STM32L1xx_256.FLM'          # L151RC = 256 KB
FLASH_SIZE = '40000'                    # 256 KB, 与 -FL040000 对应


def keil_roots():
    """猜 Keil 装在哪儿，好让这个脚本换台电脑也能用（不写死本机路径）。"""
    roots = []
    try:
        import winreg
        # 本机实测: HKLM\SOFTWARE\WOW6432Node\Keil\Products\MDK 的 Path = C:\Keil_v5\ARM
        for hive, key in ((winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\WOW6432Node\Keil\Products\MDK'),
                          (winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\Keil\Products\MDK')):
            try:
                with winreg.OpenKey(hive, key) as k:
                    p = winreg.QueryValueEx(k, 'Path')[0]
                    roots.append(os.path.dirname(os.path.normpath(p)))
            except OSError:
                pass
    except ImportError:
        pass

    for d in ('C:', 'D:', 'E:'):
        roots += [d + '\\Keil_v5', d + '\\Keil', d + '\\MDK']
    la = os.environ.get('LOCALAPPDATA')
    if la:
        roots.append(os.path.join(la, 'Arm', 'Packs'))
    seen, out = set(), []
    for r in roots:
        if r and r.lower() not in seen and os.path.isdir(r):
            seen.add(r.lower())
            out.append(r)
    return out


def find_flm(explicit=None):
    """找出烧写算法 .FLM 的绝对路径（装的是 Keil.STM32L1xx_DFP 器件包）。"""
    if explicit:
        return explicit if os.path.isfile(explicit) else None
    if os.path.isfile(FLM_FALLBACK):
        return FLM_FALLBACK

    pats = []
    for r in keil_roots():
        pats.append(os.path.join(r, 'ARM', 'PACK', '**', FLM_NAME))
        pats.append(os.path.join(r, '**', FLM_NAME))
    hits = []
    for p in pats:
        hits += glob.glob(p, recursive=True)
    hits = sorted(set(hits))
    return hits[-1] if hits else None


def read(p):
    with open(p, encoding='utf-8', errors='replace', newline='') as f:
        return f.read()


def write(p, s):
    with open(p, 'w', encoding='utf-8', newline='') as f:
        f.write(s)


def backup(p):
    b = p + '.bak-prep'
    if not os.path.exists(b):
        shutil.copy2(p, b)
        return b
    return None


# ---------- ① IncludePath ----------
def fix_include_path(text):
    added = []

    def sub(m):
        val = m.group(1)
        if not val.strip() or 'CMSIS\\Include' in val:
            return m.group(0)
        added.append(val[-60:])
        return '<IncludePath>%s;%s</IncludePath>' % (val.rstrip(';'), CMSIS_REL)

    return re.sub(r'<IncludePath>(.*?)</IncludePath>', sub, text, flags=re.S), added


# ---------- ② -FP0 / ③ -FO ----------
def fix_driver_string(s, flm):
    notes = []
    if '-FP0(' not in s:
        # 只给声明了 256 KB 的那些补（-FL040000）；别的容量该用别的 FLM，别乱补
        if re.search(r'-FL0?%s' % FLASH_SIZE.replace('0000', '0000'), s) or '-FL040000' in s:
            s = s.rstrip()
            if s.endswith(')'):
                s = s[:-1] + ' -FP0(%s))' % flm
                notes.append('补 -FP0')
            else:
                s = s + ' -FP0(%s)' % flm
                notes.append('补 -FP0')
        else:
            notes.append('!! 缺 -FP0 但容量不是 256K，跳过')
    if '-FO7 ' in s:
        s = s.replace('-FO7 ', '-FO15 ', 1)
        notes.append('-FO7 -> -FO15')
    return s, notes


def fix_uvproj(text, flm):
    text, inc = fix_include_path(text)
    notes = []
    if inc:
        notes.append('IncludePath 补了 %d 处' % len(inc))

    def sub(m):
        s, n = fix_driver_string(m.group(2), flm)
        if n:
            notes.append('<FlashDriverDll>%s: %s' % (m.group(2)[:8], ', '.join(n)))
        return m.group(1) + s + m.group(3)

    text = re.sub(r'(<FlashDriverDll>)(.*?)(</FlashDriverDll>)', sub, text, flags=re.S)
    return text, notes


def fix_uvopt(text, flm):
    notes = []

    def sub(m):
        s = m.group(2)
        # target 1 是唯一用 256K 算法的那条，别的 target 各有自己的 $$Device 路径
        if '-FF0STM32L1xx_256' not in s:
            return m.group(0)
        s2, n = fix_driver_string(s, flm)
        if n:
            notes.append('target1 驱动串: %s' % ', '.join(n))
        return m.group(1) + s2 + m.group(3)

    text = re.sub(r'(<Key>ST-LINKIII-KEIL_SWO</Key>\s*<Name>)(.*?)(</Name>)', sub, text, flags=re.S)
    return text, notes


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    arg = sys.argv[1]
    check_only = '--check' in sys.argv
    flm_arg = None
    for i, a in enumerate(sys.argv):
        if a == '--flm' and i + 1 < len(sys.argv):
            flm_arg = sys.argv[i + 1]
        elif a.startswith('--flm='):
            flm_arg = a.split('=', 1)[1]

    if arg.lower().endswith('.uvproj'):
        uvproj = arg
        base = os.path.dirname(arg)
    else:
        base = arg
        uvproj = os.path.join(arg, 'Project.uvproj')
    uvopt = os.path.join(base, 'Project.uvopt')

    if not os.path.isfile(uvproj):
        print('找不到工程文件: %s' % uvproj)
        return 2

    flm = find_flm(flm_arg)
    print('工程: %s' % uvproj)
    print('算法: %s' % (flm or '*** 没找到 %s ***' % FLM_NAME))
    if not flm:
        roots = keil_roots()
        print('  找过这些 Keil 根目录: %s' % (', '.join(roots) if roots else '(一个都没有)'))
        print('  先装 Keil.STM32L1xx_DFP 器件包（换电脑时把 .pack 文件一起带过去装），')
        print('  或者手工指定: --flm "<某处>\\STM32L1xx_256.FLM"')
        if not check_only:
            return 2
    print('模式: %s' % ('只检查' if check_only else '检查并修复'))
    print('')

    t = read(uvproj)
    t2, n1 = fix_uvproj(t, flm)
    changed = (t2 != t)

    u2 = None
    n2 = []
    if os.path.isfile(uvopt):
        u = read(uvopt)
        u2, n2 = fix_uvopt(u, flm)
        if u2 != u:
            changed = True
    else:
        print('!! 没有 Project.uvopt —— Keil 第一次用界面打开时才会生成它。')
        print('   命令行烧录需要它。先在 Keil 界面里打开一次这个工程并保存。')
        print('')

    for x in n1 + n2:
        print('  · %s' % x)
    if not n1 and not n2:
        print('  已经是对的，无需修改。')
    print('')

    if check_only:
        print('结论: %s' % ('有需要修的地方（去掉 --check 执行修复）' if changed else 'OK'))
        return 0 if not changed else 1

    if not changed:
        return 0

    b1 = backup(uvproj)
    write(uvproj, t2)
    print('已写入 %s   (备份 %s)' % (os.path.basename(uvproj), b1 or '已存在，未覆盖'))
    if u2 is not None:
        b2 = backup(uvopt)
        write(uvopt, u2)
        print('已写入 %s   (备份 %s)' % (os.path.basename(uvopt), b2 or '已存在，未覆盖'))
    print('')
    print('下一步: powershell -NoProfile -ExecutionPolicy Bypass -File flash_and_run.ps1 -Uvproj "%s"' % uvproj)
    return 0


if __name__ == '__main__':
    sys.exit(main())
