#!/usr/bin/env python3
"""
at_console.py - send AT commands to the BC28 and show its raw reply.

Use it together with the AT-bridge firmware (bc28-bridge project): that firmware
makes the STM32 shovel bytes between USB1 (CH340) and the module's MAIN uart, so
this script can talk to the modem over the normal COM port. Without the bridge
firmware the module is unreachable from the PC - see the manual, part 2.

    python at_console.py AT "AT+QCCID" "AT+CSQ"
    python at_console.py -w 3 "AT+CUSD=1,\"*100#\""      (3 s wait, slow answer)

Options:
    -p/--port   COM port (default: auto - the one CH340/1A86 port)
    -b/--baud   default 9600 (must match the bridge firmware)
    -w/--wait   seconds to listen after each command (default 2)
    --no-echo   do not print the "[sent] ..." line

Exit code: 0 if every command got a non-empty reply, 1 otherwise.
"""

import sys
import time
import argparse

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is missing.  Install it with:  pip install pyserial")

BAUD_DEFAULT = 9600
CH340_VID = 0x1A86


def pick_port(explicit):
    """Return the COM port to use: the given one, else the single CH340 port."""
    if explicit:
        return explicit
    ports = sorted(list_ports.comports(), key=lambda p: p.device)
    if not ports:
        sys.exit("no COM port present - is USB1 (the board's own USB socket) plugged in?")
    wch = [p for p in ports if p.vid == CH340_VID]
    if len(wch) == 1:
        return wch[0].device
    if len(ports) == 1:
        return ports[0].device
    if wch:
        print(f"several CH340 ports, taking {wch[0].device}; use -p to choose "
              f"(all: {', '.join(p.device for p in ports)})")
        return wch[0].device
    print(f"no CH340 found, taking {ports[0].device} "
          f"(all: {', '.join(p.device for p in ports)})")
    return ports[0].device


def listen(port, seconds):
    """Read whatever arrives within `seconds`, return it as text."""
    end = time.time() + seconds
    chunks = []
    while time.time() < end:
        n = port.in_waiting
        if n:
            chunks.append(port.read(n))
            end = time.time() + 0.35          # keep reading while it flows
        else:
            time.sleep(0.02)
    return b"".join(chunks).decode("ascii", "replace")


def show(text):
    """Print the reply line by line, stripping the trailing CR the modem sends."""
    lines = [ln.rstrip("\r") for ln in text.replace("\r\n", "\n").split("\n")]
    while lines and lines[-1] == "":
        lines.pop()
    for ln in lines:
        print(f"  | {ln}" if ln else "  |")
    return [ln for ln in lines if ln.strip()]


def main():
    ap = argparse.ArgumentParser(add_help=True, description="send AT commands via the AT bridge")
    ap.add_argument("cmds", nargs="+", help='commands, e.g. AT "AT+QCCID" (quotes if it has spaces/commas)')
    ap.add_argument("-p", "--port")
    ap.add_argument("-b", "--baud", type=int, default=BAUD_DEFAULT)
    ap.add_argument("-w", "--wait", type=float, default=2.0)
    ap.add_argument("--no-echo", action="store_true")
    args = ap.parse_args()

    dev = pick_port(args.port)
    print(f"port {dev} @ {args.baud}  (bridge firmware must be running)")

    with serial.Serial(dev, args.baud, timeout=0.1) as port:
        time.sleep(0.3)
        port.reset_input_buffer()

        # Wake-up: the module echoes this once it is listening. Also clears any
        # half-typed line left over from a previous session.
        port.write(b"AT\r\n")
        time.sleep(0.5)
        hello = show(listen(port, 0.6))
        if not hello:
            print("  !! no answer to a plain AT - bridge firmware running? "
                  "USB1 plugged in? module powered?")
        print()

        ok = True
        for cmd in args.cmds:
            if not args.no_echo:
                print(f"[sent] {cmd}")
            port.write(cmd.encode("ascii", "replace") + b"\r\n")
            reply = show(listen(port, args.wait))
            if not reply:
                ok = False
            print()

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
