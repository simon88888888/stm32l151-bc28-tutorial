# Build + flash a Keil project with its CLI, then capture the board's serial log.
#
# GENERIC: pass any Keil project - a .uvproj path, or just its directory.
#   flash_and_run.ps1 D:\path\to\AnotherProject\Project\Test\MDK-ARM
#   flash_and_run.ps1 D:\path\to\AnotherProject\Project\Test\MDK-ARM\Project.uvproj
# With no argument it looks for a Project.uvproj under the current directory, and
# only if it finds none does it fall back to the hello-world project.
# If the tree holds SEVERAL projects it lists them (newest build output first) and
# asks which one; Enter = [1] = the newest. Add -Yes to skip the question and always
# take the newest (also the automatic behaviour when stdin is not a console, e.g.
# when this is driven from another script).
#
# WHICH FILE GETS FLASHED: not one you name - Keil flashes the project's own build
# output, <OutputName>.axf in its <OutputDirectory>. To flash a different program you
# point at a different PROJECT (or a different -Target inside it). That is the only
# way to "specify the file"; Keil's CLI cannot flash an arbitrary .hex/.axf.
#
# Keil's -f only DOWNLOADS; it does not compile. So this script builds first (-b)
# unless you pass -NoBuild. Exit codes: 0 ok, 1 warnings, >=2 errors.
#
# A vendor example needs BOTH of these in its target-1 driver string first, or it
# fails in two different and very confusing ways (run prep_project.py to apply):
#   -FP0(<path to STM32L1xx_256.FLM>)   missing -> "Erase Failed!" on a healthy chip
#   -FO15  (not -FO7)                   missing -> downloads fine but the chip is left
#                                       halted, so the serial stays silent (-FO15 is
#                                       Keil's "Reset and Run")
# Also: Keil must be CLOSED - it holds the ST-Link and the CLI then cannot connect.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File flash_and_run.ps1
#   powershell ... -File flash_and_run.ps1 <project> -Seconds 10
#   powershell ... -File flash_and_run.ps1 <project> -NoBuild      # flash last build
#   powershell ... -File flash_and_run.ps1 -SkipFlash              # just read the log
# Pure ASCII (PowerShell 5.1 reads BOM-less .ps1 as GBK).
param(
  [Parameter(Position = 0)]
  [Alias('Uvproj', 'Project', 'Proj')]
  [string]$ProjArg = "",        # .uvproj path, or a directory containing one
  [string]$Target = "STM32L1XX_MD(STM32L1xxxBxx)",
  [string]$Uv4    = "",          # empty = auto-detect Keil's UV4.exe
  [string]$Port   = "",          # empty = auto-detect first COM port
  [int]$Seconds   = 6,
  [int]$Baud      = 9600,
  [string]$HexCopyTo = "",       # optional: also copy the built .hex somewhere handy
  [string]$DefaultProject = "D:\claudecode\stm32-bc28\print-helloworld\Project\Test\MDK-ARM\Project.uvproj",
  [switch]$SkipFlash,
  [switch]$NoBuild,
  [switch]$Yes         # several projects found -> don't ask, take the newest
)

# ---------- resolve Keil's UV4.exe (no hard-coded machine paths) ----------
function Find-Uv4 {
  $cands = @()
  try {
    # 本机实测: HKLM\SOFTWARE\WOW6432Node\Keil\Products\MDK 的 Path = C:\Keil_v5\ARM
    foreach ($k in @('HKLM:\SOFTWARE\WOW6432Node\Keil\Products\MDK', 'HKLM:\SOFTWARE\Keil\Products\MDK')) {
      $p = (Get-ItemProperty -Path $k -ErrorAction SilentlyContinue).Path
      if ($p) {
        $root = Split-Path $p.TrimEnd('\') -Parent      # C:\Keil_v5\ARM -> C:\Keil_v5
        $cands += (Join-Path $root 'UV4\UV4.exe')
      }
    }
  } catch { }
  foreach ($d in @('C:', 'D:', 'E:')) { $cands += "$d\Keil_v5\UV4\UV4.exe"; $cands += "$d\Keil\UV4\UV4.exe" }
  $cands += (Join-Path $env:LOCALAPPDATA 'Keil_v5\UV4\UV4.exe')
  foreach ($c in $cands) { if ($c -and (Test-Path $c)) { return $c } }
  $cmd = Get-Command UV4.exe -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  return $null
}

# (resolved lazily, further down, and only when actually flashing: -SkipFlash must
#  keep working on a machine that has no Keil at all, e.g. just reading the log)

# ---------- resolve which project ----------
function Find-Projects([int]$maxUp, [int]$maxDown) {
  # A vendor project is always <...>\MDK-ARM\Project.uvproj. Two cheap passes:
  #   1) walk UP from here - catches "the .bat was dropped right next to the project"
  #   2) walk DOWN at increasing depth, matching the \MDK-ARM\Project.uvproj suffix -
  #      the shallowest depth wins, which also avoids the decoy projects that ship
  #      inside the lib (Project\STM32L1xx_StdPeriph_Examples\...\MDK-ARM\).
  $hits = @()
  for ($u = 0; $u -le $maxUp; $u++) {
    $h = Join-Path ('.' + ('\..' * $u)) 'Project.uvproj'
    $f = @(Get-ChildItem -Path $h -File -ErrorAction SilentlyContinue)
    if ($f.Count -gt 0) { $hits += $f; break }     # nearest ancestor wins
  }
  if ($hits.Count -eq 0) {
    foreach ($d in 0..$maxDown) {
      $f = @(Get-ChildItem -Path (('.\' + ('*\' * $d)) + 'MDK-ARM\Project.uvproj') -File -ErrorAction SilentlyContinue |
             Where-Object { $_.FullName -notmatch '_Examples' })
      if ($f.Count -gt 0) { $hits = $f; break }    # shallowest depth wins
    }
  }
  return $hits
}

function Get-ProjectArtifact($uvprojPath) {
  # The newest build output of a project: <OutputDirectory>\<OutputName>.axf (or .hex).
  # Falls back to the .uvproj's own timestamp for a project that was never built.
  $dir = Split-Path $uvprojPath
  try { $x = Get-Content $uvprojPath -Raw } catch { return (Get-Item $uvprojPath) }
  $name = ([regex]::Match($x, '<OutputName>(.*?)</OutputName>')).Groups[1].Value
  $od = ([regex]::Match($x, '<OutputDirectory>(.*?)</OutputDirectory>')).Groups[1].Value
  if (-not $name) { return (Get-Item $uvprojPath) }
  if (-not $od) { $od = ".\$name\" }
  $outDir = Join-Path $dir ($od -replace '^\.\\', '')
  $best = $null
  foreach ($ext in @('.axf', '.hex')) {
    $f = Join-Path $outDir ($name + $ext)
    if (Test-Path $f) {
      $i = Get-Item $f
      if (-not $best -or $i.LastWriteTime -gt $best.LastWriteTime) { $best = $i }
    }
  }
  if (-not $best) { $best = Get-Item $uvprojPath }
  return $best
}

if ($SkipFlash) {
  # pure log reader - a project is irrelevant, and so is whether Keil is even installed
  $ProjArg = ""
} elseif (-not $ProjArg) {
  $found = @(Find-Projects 6 6)
  if ($found.Count -eq 0) {
    # some other layout - scan the whole subtree, still skipping the lib's decoy examples
    $found = @(Get-ChildItem -Path . -Filter Project.uvproj -Recurse -Depth 8 -File -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -notmatch '_Examples' })
  }
  if ($found.Count -eq 1) {
    $ProjArg = $found[0].FullName
    "auto-detected project under the current directory:"
    "  $ProjArg"
  } elseif ($found.Count -gt 1) {
    # Several projects in the tree -> list them (newest build output first) and let the
    # human pick. Press Enter (= #1) or run with -Yes to just take the newest.
    $ranked = @($found | ForEach-Object {
      [pscustomobject]@{ Proj = $_.FullName; Art = (Get-ProjectArtifact $_.FullName) }
    } | Sort-Object { $_.Art.LastWriteTime } -Descending)

    "several projects found under the current directory (newest build output first):"
    for ($i = 0; $i -lt $ranked.Count; $i++) {
      "  [{0}] {1}  {2}" -f ($i + 1), $ranked[$i].Art.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'), $ranked[$i].Proj
      "      built artifact: $($ranked[$i].Art.Name)"
    }
    ""
    $pick = 1
    if ($Yes -or [Console]::IsInputRedirected) {
      # no human at the keyboard (piped / scheduled / -Yes) -> just take the newest
      $why = 'stdin is not a console'
      if ($Yes) { $why = '-Yes was given' }
      "taking [1] (the newest), not asking - $why"
    } else {
      $ans = Read-Host "which one?  [1-$($ranked.Count)], Enter = 1 (newest)"
      if ($ans -match '^\s*$') { $pick = 1 }
      elseif ($ans -match '^\s*(\d+)\s*$' -and [int]$ans -ge 1 -and [int]$ans -le $ranked.Count) { $pick = [int]$ans }
      else { "not a valid choice - using [1]"; $pick = 1 }
    }
    $ProjArg = $ranked[$pick - 1].Proj
    ""
    "chosen [$pick]: $ProjArg"
    "  (artifact: $($ranked[$pick - 1].Art.FullName), $($ranked[$pick - 1].Art.LastWriteTime))"
  } elseif (Test-Path $DefaultProject -PathType Leaf) {
    $ProjArg = $DefaultProject
    "no project argument and none under the current directory - falling back to the hello project:"
    "  $ProjArg"
  } else {
    "ABORT: no project found."
    "  Pass one:   flash_and_run.ps1 `"<project dir or .uvproj>`""
    "  Or drop (a copy of) the .bat into the project folder and double-click it there."
    exit 1
  }
}
if (-not $SkipFlash) {
  if (Test-Path $ProjArg -PathType Container) {
    $cand = Join-Path $ProjArg 'Project.uvproj'
    if (-not (Test-Path $cand)) { "ABORT: no Project.uvproj in directory: $ProjArg"; exit 1 }
    $ProjArg = $cand
  }
  if (-not (Test-Path $ProjArg -PathType Leaf)) { "ABORT: project not found: $ProjArg"; exit 1 }
  $Uvproj = (Resolve-Path $ProjArg).Path
  $ProjDir = Split-Path $Uvproj
  "project: $Uvproj"
  ""
}

# ---------- build ----------
function ShowLog($path) {
  # Write-Host, not bare strings: a function's output stream IS its return value, so
  # these lines would otherwise be glued onto the exit code RunUv4 returns.
  if (Test-Path $path) {
    (Get-Content $path -Raw) -split "`r?`n" | Where-Object { $_.Trim() } | ForEach-Object { Write-Host "  $_" }
  }
}

function RunUv4([string]$mode, [string]$logName) {
  $p = Start-Process -FilePath $Uv4 -ArgumentList @($mode, $Uvproj, "-t", $Target, "-j0", "-o", $logName) -WorkingDirectory $ProjDir -Wait -PassThru
  ShowLog (Join-Path $ProjDir $logName)
  return [int]$p.ExitCode
}

function GetOutputHex() {
  # Read <OutputName>/<OutputDirectory> so this works for any project, not just hello.
  $x = Get-Content $Uvproj -Raw
  $name = ([regex]::Match($x, '<OutputName>(.*?)</OutputName>')).Groups[1].Value
  $dir = ([regex]::Match($x, '<OutputDirectory>(.*?)</OutputDirectory>')).Groups[1].Value
  if (-not $name) { return $null }
  if (-not $dir) { $dir = ".\$name\" }
  return Join-Path $ProjDir (($dir -replace '^\.\\', '') + $name + '.hex')
}

if (-not $SkipFlash) {
  if (-not $Uv4) {
    $Uv4 = Find-Uv4
    if (-not $Uv4) {
      "ABORT: cannot find Keil's UV4.exe."
      "        Install Keil MDK, or pass its path:  -Uv4 `"C:\...\UV4\UV4.exe`""
      exit 1
    }
    "keil: $Uv4"
  }

  if (Get-Process -Name UV4 -ErrorAction SilentlyContinue) {
    "ABORT: Keil (UV4.exe) is running and holds the ST-Link. Close it first."
    exit 1
  }

  if (-not $NoBuild) {
    "--- building ---"
    $rc = RunUv4 "-b" "build.log"
    "  rc=$rc  (0 = ok, 1 = warnings, >=2 = errors)"
    if ($rc -ge 2) { "ABORT: build failed - not flashing."; exit $rc }

    if ($HexCopyTo) {
      $h = GetOutputHex
      if ($h -and (Test-Path $h)) { Copy-Item $h $HexCopyTo -Force; "  hex copy refreshed: $HexCopyTo" }
      else { "  (no .hex produced - is <CreateHexFile> off?)" }
    }
  }

  "--- flashing ---"
  $rc = RunUv4 "-f" "flash.log"
  "  rc=$rc  (0 = ok)"
  $flog = Join-Path $ProjDir "flash.log"
  if ((Test-Path $flog) -and -not (Select-String -Path $flog -Pattern "Application running" -Quiet)) {
    "  NOTE: log has no 'Application running' -> the chip was probably left halted"
    "        (this project's target-1 driver string should have -FO15, not -FO7;"
    "         run: python D:\windows11-tools\stm32-bc28\prep_project.py <this project dir>)"
  }
}

# ---------- read the serial log ----------
if (-not $Port) {
  $names = @([System.IO.Ports.SerialPort]::GetPortNames())
  if ($names.Count -eq 0) { "ABORT: no COM port - plug the board's USB1 (CH340) cable in."; exit 1 }
  $Port = $names[0]
}

$p = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$p.ReadTimeout = 500
$p.DtrEnable = $false       # BOOT0 low -> boot the application, not the bootloader
$p.RtsEnable = $false
try { $p.Open() } catch { "ABORT: cannot open $Port (another tool holding it?): $_"; exit 1 }
$p.NewLine = "`r`n"

function Capture($port, [int]$secs) {
  $end = (Get-Date).AddSeconds($secs); $n = 0
  while ((Get-Date) -lt $end) {
    try { $l = $port.ReadLine(); Write-Host "RX> $l"; $n++ } catch { }
  }
  return $n
}

Start-Sleep -Milliseconds 1200
$p.DiscardInBuffer()
"--- reading $Port for $Seconds s (no reset) ---"
$n = Capture $p $Seconds

if ($n -eq 0) {
  "--- nothing yet; pulsing RTS (reset) and reading again ---"
  $p.RtsEnable = $true; Start-Sleep -Milliseconds 200; $p.RtsEnable = $false
  $n = Capture $p $Seconds
}

$p.Close()
"--- lines=$n ---"
if ($n -eq 0) { "Still silent: check the board's power switch (the POW LED must be lit)." }
