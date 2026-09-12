# Read the STM32L151-BC28 board's USB1 log (CH340 @ 9600 8-N-1).
#   powershell -NoProfile -ExecutionPolicy Bypass -File read_com.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File read_com.ps1 -Port COM7 -Seconds 20
# No COM port at all? It explains the three possible causes and waits -WaitPort seconds
# (default 20) for you to plug USB1 in, so a late plug-in still captures the log.
# Keep this file pure ASCII: PowerShell 5.1 reads BOM-less .ps1 as GBK,
# non-ASCII characters corrupt string terminators -> ParserError.
param(
  [string]$Port = "",      # empty = auto-detect the first COM port
  [int]$Seconds = 12,
  [int]$Baud = 9600,
  [int]$WaitPort = 20      # no port present -> wait this long for one to appear
)
$ErrorActionPreference = "Stop"

if (-not $Port) {
  $names = @([System.IO.Ports.SerialPort]::GetPortNames())
  if ($names.Count -eq 0) {
    # Zero COM ports seen by Windows. From the vendor schematic: the CH340G's VCC is fed
    # straight from USB1's VBUS (net VCC_USB), i.e. BEFORE the power-select slide switch.
    # So the port's existence depends on the CABLE only - never on the switch position.
    #   no port at all       -> USB1 is not on a real USB host, or D+/D- are broken
    #   port but zero bytes  -> the MCU is not running: switch not seated (POW led dark)
    #                           or flow control is ON
    "NO COM PORT PRESENT. The CH340 is powered straight off USB1's VBUS, so this means:"
    "  - USB1 is not plugged into THIS PC. A phone charger / power bank / switched-off hub"
    "    does not count - the port only shows up on a real USB host (this PC)."
    "  - or the cable is charge-only / broken: VBUS gets through but D+/D- do not."
    "  - or the PC's USB port itself is dead (try another port)."
    "Note: the ST-Link and USB1 are two separate cables. The board's power-select switch"
    "      does NOT cause this - with a good cable in, the port appears regardless."
    ""
    "Waiting up to $WaitPort s for a port to appear - plug USB1 in now ..."
    $deadline = (Get-Date).AddSeconds($WaitPort)
    while ((Get-Date) -lt $deadline -and $names.Count -eq 0) {
      Start-Sleep -Milliseconds 800
      $names = @([System.IO.Ports.SerialPort]::GetPortNames())
    }
    if ($names.Count -eq 0) {
      "Still nothing after $WaitPort s. Nothing was plugged in - giving up."
      exit 1
    }
    "port appeared: $($names -join ',')"
    ""
  }
  $Port = $names[0]
  "auto-detected port: $Port   (all: $($names -join ','))"
}

$p = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$p.ReadTimeout = 3000
$p.NewLine = "`r`n"

# This board's auto-download circuit wires DTR/RTS into the MCU:
#   RTS HIGH = NRST reset        DTR HIGH = BOOT0 (enter bootloader)
# So if anything asserts RTS on open, the board sits in reset forever and you get
# "port opens fine but not a single byte" - the classic false alarm.
# Force both OFF (low) so reading the log can never disturb the running program.
# Measured 2026-09-12: opening the port does NOT reset the board - a second capture
# resumed at "Hello World #227" with no boot banner. These two lines make that a
# guarantee rather than a lucky default.
$p.DtrEnable = $false
$p.RtsEnable = $false

try { $p.Open() } catch { "OPEN FAILED (is SSCOM/another serial tool holding it?): $_"; exit 1 }
"opened $Port @ $Baud, capturing $Seconds s ..."
$end = (Get-Date).AddSeconds($Seconds)
$n = 0
while ((Get-Date) -lt $end) {
  try { $line = $p.ReadLine(); "RX> $line"; $n++ } catch { }
}
$p.Close()
"lines received: $n"
# Expect ~1 line/sec of "Hello World  #N" from the hello-world build.
# Zero lines but the port opened -> program not running, or flow control is ON
# (must be "none": DTR/RTS can hold the STM32 in reset / pull BOOT0).
