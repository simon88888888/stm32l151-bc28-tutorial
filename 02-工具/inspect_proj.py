# Report what would block a given vendor example from building + flashing.
import re, sys, os

base = sys.argv[1]
uvproj = os.path.join(base, 'Project.uvproj')
uvopt = os.path.join(base, 'Project.uvopt')
t = open(uvproj, encoding='utf-8', errors='replace').read()

print('=== targets ===')
for m in re.finditer(r'<TargetName>(.*?)</TargetName>', t):
    print('  ' + m.group(1))

print('=== device (first target) ===')
m = re.search(r'<Device>(.*?)</Device>', t)
print('  ' + (m.group(1) if m else '?'))

print('=== IncludePath ===')
for i, m in enumerate(re.finditer(r'<IncludePath>(.*?)</IncludePath>', t), 1):
    v = m.group(1)
    if not v.strip():
        print('  [%d] (empty - Keil4 dead field)' % i)
        continue
    has_cmsis = 'CMSIS\\Include' in v
    print('  [%d] CMSIS\\Include: %s' % (i, 'YES' if has_cmsis else '*** MISSING ***'))
    if not has_cmsis:
        print('      ' + v[-140:])

print('=== flash driver / ST-Link entries ===')
for m in re.finditer(r'<FlashDriverDll>(.*?)</FlashDriverDll>', t, re.S):
    s = m.group(1)
    print('  %-8s -FP0:%s  -FO:%s' % (s[:8], 'yes' if '-FP0(' in s else '*** NO ***',
                                        (re.search(r'-FO(\d+)', s) or ['', '?'])[1]))

if os.path.exists(uvopt):
    u = open(uvopt, encoding='utf-8', errors='replace').read()
    n = len(re.findall(r'<Key>ST-LINKIII-KEIL_SWO</Key>', u))
    print('  uvopt ST-LINKIII entries: %d' % n)
    for m in re.finditer(r'<Key>ST-LINKIII-KEIL_SWO</Key>\s*<Name>(.*?)</Name>', u, re.S):
        s = m.group(1)
        print('    -FP0:%s  -FO:%s' % ('yes' if '-FP0(' in s else '*** NO ***',
                                       (re.search(r'-FO(\d+)', s) or ['', '?'])[1]))
else:
    print('  no uvopt (Keil will create one on first GUI open -> defaults, no -FP0)')

print('=== lib layout (for the IncludePath fix) ===')
d = base
for _ in range(4):
    d = os.path.dirname(d)
    cand = os.path.join(d, 'Libraries', 'CMSIS', 'Include')
    if os.path.isdir(cand):
        rel = os.path.relpath(cand, base).replace('/', '\\')
        print('  found: Libraries\\CMSIS\\Include  -> relative path: %s' % rel)
        break
else:
    print('  NOT FOUND within 4 levels up')
