# Probe why USART1 went silent.
#   Phase 1  listen with DTR/RTS released low        -> app should print "Hello World #N"
#   Phase 2  send 0x7F, look for 0x79                -> 0x79 proves the chip is in the
#                                                       BUILT-IN BOOTLOADER (BOOT0 = high)
#   Phase 3  pulse RTS (reset) with DTR low, listen  -> proves the app boots when BOOT0 low
# Pure ASCII (PowerShell 5.1 reads BOM-less .ps1 as GBK).
param([string]$Port = "COM6", [int]$Baud = 9600)

function Drain($p, [int]$ms) {
  $list = New-Object System.Collections.Generic.List[byte]
  $end = (Get-Date).AddMilliseconds($ms)
  while ((Get-Date) -lt $end) {
    $n = $p.BytesToRead
    if ($n -gt 0) {
      $tmp = New-Object byte[] $n
      $r = $p.Read($tmp, 0, $n)
      for ($i = 0; $i -lt $r; $i++) { $list.Add($tmp[$i]) }
    } else { Start-Sleep -Milliseconds 50 }
  }
  return ,$list
}

function Show($tag, $buf) {
  $hex = ($buf | ForEach-Object { $_.ToString("X2") }) -join " "
  $txt = -join ($buf | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { "." } })
  "[$tag] bytes=$($buf.Count)"
  "        hex: $hex"
  "        txt: $txt"
}

$p = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$p.ReadTimeout = 500
$p.WriteTimeout = 500
$p.DtrEnable = $false      # BOOT0 low  (normal boot)
$p.RtsEnable = $false      # NRST released
try { $p.Open() } catch { "OPEN FAILED: $_"; exit 1 }
"opened $Port @ $Baud; DTR=low (BOOT0 low), RTS=low (no reset)"
""
Start-Sleep -Milliseconds 300
$p.DiscardInBuffer()

"--- Phase 1: passive listen 2.5s (is the app printing?) ---"
Show "P1" (Drain $p 2500)

"--- Phase 2: send 0x7F, listen 2s (0x79 = in bootloader) ---"
$p.Write([byte[]](0x7F), 0, 1)
Start-Sleep -Milliseconds 200
Show "P2" (Drain $p 2000)

"--- Phase 3: pulse RTS (reset), DTR stays low, listen 3s ---"
$p.RtsEnable = $true
Start-Sleep -Milliseconds 200
$p.RtsEnable = $false
Show "P3" (Drain $p 3000)

$p.Close()
"done"
