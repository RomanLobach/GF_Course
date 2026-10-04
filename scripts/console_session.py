#!/usr/bin/env python3
"""Send console commands to a device over serial and record the transcript.

usage: console_session.py PORT OUTFILE [--boot] "cmd1" "cmd2" ...
  --boot   pulse RTS to reset the board first and capture the boot log
  a command "@wait N" just records output for N seconds
"""
import sys, time, serial

def drain(s, secs, sink=None):
    """Read for `secs` (extended while data keeps coming, capped at 4x). With a sink every
    chunk is written there immediately, so a disconnect mid-stream loses nothing."""
    start, end, out = time.time(), time.time() + secs, b""
    while time.time() < min(end, start + 4 * secs):
        try:
            chunk = s.read(4096)
        except serial.SerialException:
            if sink: sink("\n### [port lost]")
            break
        if chunk:
            out += chunk
            if sink: sink(chunk.decode("utf-8", "replace"), raw=True)
            end = max(end, time.time() + 0.4)
    return out.decode("utf-8", "replace")

def main():
    port, outfile, *cmds = sys.argv[1:]
    s = serial.Serial(port, 115200, timeout=0.1)
    f = open(outfile, "a", buffering=1)
    def emit(text, raw=False):
        f.write(text if raw else text + "\n"); print(text, end="" if raw else "\n", flush=True)
    if cmds and cmds[0] == "--boot":
        cmds = cmds[1:]
        s.dtr = False; s.rts = True; time.sleep(0.2); s.rts = False
        emit("### [reset]"); drain(s, 6, emit)
    else:
        drain(s, 0.5)
    for c in cmds:
        if c.startswith("@wait"):
            secs = float(c.split()[1])
            end = time.time() + secs
            while time.time() < end:
                try:
                    chunk = s.read(4096)
                except serial.SerialException:
                    emit("\n### [port lost]"); return
                if chunk:
                    emit(chunk.decode("utf-8", "replace"), raw=True)
            continue
        s.write((c + "\n").encode())
        emit(f"> {c}"); drain(s, 2.5, emit)

main()
