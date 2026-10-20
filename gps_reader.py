#!/usr/bin/env python3
"""GPS reader for u-blox M10 (SEQURE M10-25Q) connected to Raspberry Pi 5 UART0.

Wiring: GPS TX -> GPIO15 (RXD0), GPS RX -> GPIO14 (TXD0).
Steps: init UART -> configure module via UBX CFG-VALSET -> read NMEA forever.
"""

import logging
import re
import signal
import struct
import sys
import time
from logging.handlers import RotatingFileHandler
from pathlib import Path

import serial

PORT = "/dev/ttyAMA0"
BAUDRATE = 115200
LOG_FILE = Path(__file__).with_name("gps.log")
PRINT_INTERVAL = 1.0  # seconds
SILENCE_LIMIT = 3     # timeouts in a row -> module probably reset, reconfigure

# UBX CFG-VALSET configuration keys (u-blox M10 interface description)
CFG_RATE_MEAS = 0x30210001        # U2, measurement period, ms
CFG_MSGOUT_NMEA_UART1 = {         # U1, output rate per navigation epoch
    "GGA": 0x209100BB,
    "RMC": 0x209100AC,
    "GSA": 0x209100C0,
    "GSV": 0x209100C5,
    "GLL": 0x209100CA,
    "VTG": 0x209100B1,
}
LAYER_RAM = 0x01

# Module also streams binary UBX, so NMEA sentences are extracted from the raw byte stream
NMEA_RE = re.compile(rb"\$G[A-Z]{4},[ -~]*?\*[0-9A-F]{2}")

log = logging.getLogger("gps")
running = True


# ---------------------------------------------------------------- UART / UBX

def init_uart() -> serial.Serial:
    ser = serial.Serial(
        PORT,
        BAUDRATE,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=1.0,
    )
    ser.reset_input_buffer()
    log.info("UART opened: %s @ %d 8N1", PORT, BAUDRATE)
    return ser


def ubx_frame(cls: int, msg_id: int, payload: bytes) -> bytes:
    body = struct.pack("<BBH", cls, msg_id, len(payload)) + payload
    ck_a = ck_b = 0
    for b in body:
        ck_a = (ck_a + b) & 0xFF
        ck_b = (ck_b + ck_a) & 0xFF
    return b"\xb5\x62" + body + bytes((ck_a, ck_b))


def wait_ack(ser: serial.Serial, cls: int, msg_id: int, timeout: float = 1.5) -> bool | None:
    """True = ACK, False = NAK, None = no answer."""
    deadline = time.monotonic() + timeout
    buf = b""
    while time.monotonic() < deadline:
        buf += ser.read(ser.in_waiting or 1)
        for ack_id, result in ((0x01, True), (0x00, False)):
            if ubx_frame(0x05, ack_id, bytes((cls, msg_id))) in buf:
                return result
    return None


def configure_gps(ser: serial.Serial) -> bool:
    items = [(CFG_RATE_MEAS, struct.pack("<H", 1000))]  # 1 Hz
    for name, key in CFG_MSGOUT_NMEA_UART1.items():
        rate = 1 if name in ("GGA", "RMC") else 0
        items.append((key, struct.pack("<B", rate)))

    payload = struct.pack("<BBH", 0x00, LAYER_RAM, 0)
    for key, value in items:
        payload += struct.pack("<I", key) + value

    for attempt in range(1, 4):
        ser.write(ubx_frame(0x06, 0x8A, payload))
        ser.flush()
        result = wait_ack(ser, 0x06, 0x8A)
        if result is True:
            log.info("GPS configured: 1 Hz, NMEA GGA+RMC only (RAM layer)")
            return True
        log.warning("CFG-VALSET attempt %d: %s", attempt, "NAK" if result is False else "no ACK")
    log.error("GPS configuration failed, continuing with module defaults")
    return False


# ---------------------------------------------------------------- NMEA

def nmea_valid(line: str) -> bool:
    if not line.startswith("$") or "*" not in line:
        return False
    data, _, cs = line[1:].partition("*")
    calc = 0
    for ch in data:
        calc ^= ord(ch)
    try:
        return calc == int(cs[:2], 16)
    except ValueError:
        return False


def nmea_coord(value: str, hemi: str) -> float | None:
    if not value:
        return None
    dot = value.index(".")
    deg = float(value[: dot - 2])
    minutes = float(value[dot - 2 :])
    coord = deg + minutes / 60.0
    return -coord if hemi in ("S", "W") else coord


class GpsState:
    def __init__(self):
        self.time = self.date = None
        self.lat = self.lon = None
        self.alt = None
        self.speed_kmh = None
        self.sats = 0
        self.hdop = None
        self.fix = 0          # GGA quality: 0 = none, 1 = GPS, 2 = DGPS ...
        self.valid = False    # RMC status A/V

    def update(self, line: str):
        fields = line[1:].split("*")[0].split(",")
        kind = fields[0][2:]
        try:
            if kind == "RMC" and len(fields) >= 10:
                self.time = fields[1][:6]
                self.valid = fields[2] == "A"
                self.lat = nmea_coord(fields[3], fields[4])
                self.lon = nmea_coord(fields[5], fields[6])
                self.speed_kmh = float(fields[7]) * 1.852 if fields[7] else None
                self.date = fields[9]
            elif kind == "GGA" and len(fields) >= 10:
                self.fix = int(fields[6] or 0)
                self.sats = int(fields[7] or 0)
                self.hdop = float(fields[8]) if fields[8] else None
                self.alt = float(fields[9]) if fields[9] else None
        except (ValueError, IndexError):
            log.debug("Bad NMEA fields: %s", line)

    def __str__(self):
        if self.date and self.time:
            d, t = self.date, self.time
            ts = f"20{d[4:6]}-{d[2:4]}-{d[0:2]} {t[0:2]}:{t[2:4]}:{t[4:6]} UTC"
        else:
            ts = "no time"
        if self.valid and self.lat is not None:
            pos = f"{self.lat:.6f}, {self.lon:.6f}"
            alt = f"{self.alt:.1f} m" if self.alt is not None else "-"
            spd = f"{self.speed_kmh:.1f} km/h" if self.speed_kmh is not None else "-"
            return f"{ts} | {pos} | alt {alt} | {spd} | sats {self.sats} | hdop {self.hdop} | FIX"
        return f"{ts} | NO FIX | sats {self.sats}"


# ---------------------------------------------------------------- main loop

def read_loop(ser: serial.Serial):
    state = GpsState()
    last_print = 0.0
    buf = b""
    silent = 0
    while running:
        chunk = ser.read(ser.in_waiting or 1)
        if not chunk:
            silent += 1
            log.warning("No data from GPS (timeout %d/%d)", silent, SILENCE_LIMIT)
            if silent >= SILENCE_LIMIT:
                log.warning("GPS silent, reopening UART and reconfiguring module")
                return
            continue
        silent = 0
        buf += chunk
        end = 0
        for m in NMEA_RE.finditer(buf):
            end = m.end()
            line = m.group().decode("ascii")
            if nmea_valid(line):
                state.update(line)
        buf = buf[end:][-4096:]
        now = time.monotonic()
        if now - last_print >= PRINT_INTERVAL:
            log.info("%s", state)
            last_print = now


def stop(signum, _frame):
    global running
    log.info("Signal %d received, stopping", signum)
    running = False


def setup_logging():
    fmt = logging.Formatter("%(asctime)s %(levelname)s %(message)s")
    stdout = logging.StreamHandler(sys.stdout)
    stdout.setFormatter(logging.Formatter("%(levelname)s %(message)s"))  # journald adds time
    file = RotatingFileHandler(LOG_FILE, maxBytes=5_000_000, backupCount=3)
    file.setFormatter(fmt)
    log.addHandler(stdout)
    log.addHandler(file)
    log.setLevel(logging.INFO)


def main():
    setup_logging()
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    log.info("GPS reader starting")

    while running:
        try:
            with init_uart() as ser:
                configure_gps(ser)
                read_loop(ser)
        except serial.SerialException as e:
            log.error("Serial error: %s, retry in 3 s", e)
            time.sleep(3)
    log.info("GPS reader stopped")


if __name__ == "__main__":
    main()
