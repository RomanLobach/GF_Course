#!/usr/bin/env python3
"""Capture one radio profile of the LoRa/FSK practicum with an RTL-SDR and render
detailed waterfall (spectrogram) pictures of it.

Made for the firmware's TEST mode ("Тест частот"): both devices sit on
one profile and the Base sends a 4-packet burst every cycle. For each capture this script
produces:

  <label>_<time>_overview.png  - the whole recording: bursts, listen gaps, service-channel
                                 slot packets, sub-band edges, the packet picked for detail
  <label>_<time>_detail.png    - one bench packet found automatically: the full packet on
                                 top, a zoom on its start below (LoRa preamble chirps /
                                 FSK preamble+sync bits), STFT tuned to the modulation
  <label>_<time>_meta.json     - capture settings and measured numbers

Usage (single profile; see run_all.py for the guided walk through all of them):
    python capture_waterfall.py --profile 3              # gain 0 dB (devices next to the SDR)
    python capture_waterfall.py --profile 3 --gain 20    # devices a few metres away
    python capture_waterfall.py --profile S            # service channel (SYNC/TEST slots)
    python capture_waterfall.py --analyze captures/.../P3_..._iq.cu8 --profile 3

Profile table mirrors include/Config.h (BENCH_CONFIGS + service channel) - keep in sync.
"""
import argparse
import datetime
import json
import math
import os
import subprocess
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from scipy import signal

RTL_SDR_BIN = os.path.expanduser("~/radioconda/bin/rtl_sdr")
DEFAULT_RATE = 2.4e6  # RTL-SDR Blog V4 is stable here; covers BW500 / FSK 100k with room to spare

# Sub-band edges, drawn on the overview when they fall inside the span.
SUBBANDS = [("G3 10%", 869.40e6, 869.65e6), ("G1 1%", 868.00e6, 868.60e6)]

# key: label, kind, centre freq, LoRa bw/sf or FSK bitrate/deviation, on-air time of one packet
# (s, 64-byte bench packet / 4-byte service packet), capture length (s) = at least one full
# TEST cycle plus one extra packet, so a complete packet is always inside the recording.
PROFILES = {
    "S": dict(label="S_SERVICE_LoRa_SF10_BW125", title="Службовий канал — LoRa SF10 / BW125",
              kind="lora", freq=869.525e6, bw=125e3, sf=10, airtime=0.207, duration=12.0),
    "1": dict(label="P1_LoRa_SF6_BW125", title="Профіль 1 — LoRa SF6 / BW125",
              kind="lora", freq=869.525e6, bw=125e3, sf=6, airtime=0.067, duration=6.0),
    "2": dict(label="P2_LoRa_SF6_BW500", title="Профіль 2 — LoRa SF6 / BW500",
              kind="lora", freq=868.300e6, bw=500e3, sf=6, airtime=0.017, duration=6.0),
    "3": dict(label="P3_LoRa_SF12_BW125", title="Профіль 3 — LoRa SF12 / BW125",
              kind="lora", freq=869.525e6, bw=125e3, sf=12, airtime=2.79, duration=18.0),
    "4": dict(label="P4_LoRa_SF12_BW500", title="Профіль 4 — LoRa SF12 / BW500",
              kind="lora", freq=868.300e6, bw=500e3, sf=12, airtime=0.616, duration=8.0),
    "5": dict(label="P5_FSK_15200", title="Профіль 5 — FSK 15.2 кбіт/с, девіація 15.2 кГц",
              kind="fsk", freq=869.525e6, bitrate=15200, dev=15200, airtime=0.036, duration=6.0),
    "6": dict(label="P6_FSK_100000", title="Профіль 6 — FSK 100 кбіт/с, девіація 100 кГц",
              kind="fsk", freq=868.300e6, bitrate=100000, dev=100000, airtime=0.006, duration=6.0),
}


def occupied_bw(p):
    """Nominal occupied bandwidth: LoRa = BW, FSK = Carson's rule 2*(dev + bitrate/2)."""
    if p["kind"] == "lora":
        return p["bw"]
    return 2 * (p["dev"] + p["bitrate"] / 2)


def tuning_offset(p):
    """Tune away from the signal so the RTL-SDR DC spike never sits on it."""
    return max(250e3, occupied_bw(p) * 0.8)


def pow2_round(x, lo=16, hi=16384):
    return int(min(hi, max(lo, 2 ** round(math.log2(max(x, 1))))))


def detail_nperseg(p, rate, zoom=False):
    """STFT window matched to the modulation.
    LoRa: a chirp is sharpest with a window of ~sqrt(Tsym / BW) (same for both panels).
    FSK, whole packet: long window (~6/deviation) - the two tones show up as two clean lines.
    FSK, zoom: short window (~2/deviation) - individual bits / tone switches become visible."""
    if p["kind"] == "lora":
        tsym = (2 ** p["sf"]) / p["bw"]
        return pow2_round(math.sqrt(tsym / p["bw"]) * rate)
    return pow2_round(rate / p["dev"] * (2.0 if zoom else 6.0))


def refine_start(x, rate, p, pad):
    """Sample-accurate packet start inside the detail slice (detection is only frame-accurate)."""
    w = max(8, int(rate * 20e-6))
    env = np.convolve(np.abs(x) ** 2, np.ones(w) / w, mode="same")
    head = env[: max(w * 4, int(pad * rate * 0.5))]
    noise = float(np.median(head))
    peak = float(np.percentile(env, 95))
    idx = np.nonzero(env > noise + 0.25 * (peak - noise))[0]
    return int(idx[0]) if idx.size else int(pad * rate)


# ---------------------------------------------------------------------------
# Capture
# ---------------------------------------------------------------------------

def capture(center, rate, duration, gain, out_iq, device_index=0, ppm=0):
    n_samples = int(rate * duration)
    cmd = [RTL_SDR_BIN, "-f", str(int(center)), "-s", str(int(rate)), "-d", str(device_index),
           "-n", str(n_samples)]
    if gain is not None:
        # rtl_sdr treats "-g 0" as automatic gain (AGC); 0.1 selects the real 0 dB manual step.
        cmd += ["-g", str(gain if gain > 0 else 0.1)]
    if ppm:
        cmd += ["-p", str(ppm)]
    cmd.append(out_iq)
    print("Запис:", " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr.strip())
        raise RuntimeError(f"rtl_sdr завершився з кодом {r.returncode}")
    lost = [l for l in r.stderr.splitlines() if "lost" in l.lower() or "short read" in l.lower()]
    if lost:
        print("УВАГА:", "; ".join(lost))


def iq_from_bytes(b):
    x = b.astype(np.float32)
    x -= 127.4
    x /= 128.0
    return x[0::2] + 1j * x[1::2]


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------

def overview(raw, rate, center, p, skip, nfft=2048, max_rows=1600):
    """Max-hold waterfall of the whole capture (short packets never average away) plus the
    per-frame power inside the profile's band, used for packet detection."""
    usable = raw.size // 2 - skip
    nframes = usable // nfft
    if nframes < 4:
        raise RuntimeError("Запис закороткий")
    fpr = max(1, nframes // max_rows)  # frames per displayed row
    rows = nframes // fpr
    nframes = rows * fpr
    win = np.hanning(nfft).astype(np.float32)
    freqs = np.fft.fftshift(np.fft.fftfreq(nfft, 1 / rate)) + center
    half = occupied_bw(p) / 2
    band = (freqs >= p["freq"] - half) & (freqs <= p["freq"] + half)

    wf = np.empty((rows, nfft), dtype=np.float32)
    band_pow = np.empty(nframes, dtype=np.float64)
    rows_per_chunk = max(1, 512 // fpr)
    for r0 in range(0, rows, rows_per_chunk):
        r1 = min(rows, r0 + rows_per_chunk)
        f0, f1 = r0 * fpr, r1 * fpr
        b = raw[2 * (skip + f0 * nfft): 2 * (skip + f1 * nfft)]
        x = iq_from_bytes(b).reshape(f1 - f0, nfft)
        X = np.fft.fftshift(np.fft.fft(x * win, axis=1), axes=1)
        P = (X.real ** 2 + X.imag ** 2).astype(np.float32)
        band_pow[f0:f1] = P[:, band].sum(axis=1)
        wf[r0:r1] = P.reshape(r1 - r0, fpr, nfft).max(axis=1)
    frame_s = nfft / rate
    return dict(wf=10 * np.log10(wf + 1e-12), freqs=freqs, row_s=fpr * frame_s, frame_s=frame_s,
                band_db=10 * np.log10(band_pow + 1e-12), nframes=nframes)


def find_packets(band_db, frame_s, p, thresh_db=8.0):
    """Runs of frames whose in-band power is well above the noise floor."""
    # Low percentile: on SF12/BW125 the packets fill >70% of a TEST capture, so a median-ish
    # estimate would land inside the signal and hide every packet.
    floor = float(np.percentile(band_db, 5))
    above = band_db > floor + thresh_db
    segs = []
    i, n = 0, len(above)
    while i < n:
        if not above[i]:
            i += 1
            continue
        j = i
        while j < n and (above[j] or (j + 2 < n and above[j + 1:j + 3].any())):
            j += 1
        segs.append((i, j))
        i = j + 1
    out = []
    for a, b in segs:
        dur = (b - a) * frame_s
        out.append(dict(start=a * frame_s, end=b * frame_s, dur=dur,
                        peak_db=float(band_db[a:b].max()), mean_db=float(band_db[a:b].mean()),
                        edge=(a == 0 or b >= n - 1)))
    return floor, out


def pick_packet(segs, p):
    exp = p["airtime"]
    good = [s for s in segs if not s["edge"] and 0.6 * exp <= s["dur"] <= 1.6 * exp + 0.003]
    if good:
        return max(good, key=lambda s: s["mean_db"]), True
    rest = [s for s in segs if not s["edge"]]
    if rest:
        return max(rest, key=lambda s: s["mean_db"]), False
    return None, False


def stft_db(x, rate, nperseg, max_cols=3000, overlap_div=8, nfft=None):
    hop = max(1, nperseg // overlap_div, len(x) // max_cols)
    hop = min(hop, nperseg)
    # nfft > nperseg zero-pads: same resolution, but chirps/tones are drawn smoothly instead of
    # in coarse frequency steps when a wide frequency span is shown.
    f, t, S = signal.spectrogram(x, fs=rate, window="hann", nperseg=nperseg, noverlap=nperseg - hop,
                                 nfft=max(nperseg, nfft or nperseg), return_onesided=False,
                                 detrend=False, scaling="spectrum", mode="psd")
    f = np.fft.fftshift(f)
    S = np.fft.fftshift(S, axes=0)
    return f, t, 10 * np.log10(S + 1e-15)


def robust_limits(img, lo=25, hi=99.95):
    return float(np.percentile(img, lo)), float(np.percentile(img, hi))


# ---------------------------------------------------------------------------
# Plots
# ---------------------------------------------------------------------------

def draw_overview(ov, center, rate, p, pkt, meta, out_png):
    wf = ov["wf"]
    f_mhz = ov["freqs"] / 1e6
    t_end = wf.shape[0] * ov["row_s"]
    vmin, vmax = robust_limits(wf, 30, 99.98)
    fig, ax = plt.subplots(figsize=(13, 8))
    im = ax.imshow(wf, aspect="auto", cmap="inferno", vmin=vmin, vmax=vmax,
                   extent=[f_mhz[0], f_mhz[-1], t_end, 0], interpolation="nearest")
    half = occupied_bw(p) / 2e6
    for x in (p["freq"] / 1e6 - half, p["freq"] / 1e6 + half):
        ax.axvline(x, color="cyan", ls="--", lw=0.8, alpha=0.8)
    for name, lo, hi in SUBBANDS:
        for x in (lo / 1e6, hi / 1e6):
            if f_mhz[0] < x < f_mhz[-1]:
                ax.axvline(x, color="white", ls=":", lw=0.8, alpha=0.6)
        mid = (lo + hi) / 2e6
        if f_mhz[0] < mid < f_mhz[-1]:
            ax.text(mid, t_end * 0.01, name, color="white", ha="center", va="top", fontsize=9, alpha=0.8)
    ax.text(center / 1e6, t_end * 0.99, " DC тюнера (не сигнал)", color="violet", ha="left", va="bottom",
            fontsize=8, alpha=0.9)
    if pkt:
        ax.axhspan(pkt["start"], pkt["end"], color="lime", alpha=0.12)
        ax.text(f_mhz[-1], pkt["start"], " ← пакет на детальному знімку", color="lime", ha="right",
                va="bottom", fontsize=9)
    ax.set_xlabel("Частота, МГц")
    ax.set_ylabel("Час, с")
    ax.set_title(f"{p['title']} — огляд запису\n"
                 f"центр {center / 1e6:.3f} МГц, {rate / 1e6:.1f} MS/s, gain {meta['gain']} дБ, "
                 f"{meta['timestamp']}   (блакитні лінії — смуга сигналу, білі — межі піддіапазонів)",
                 fontsize=10)
    fig.colorbar(im, ax=ax, label="Потужність, дБ (max-hold)")
    fig.tight_layout()
    fig.savefig(out_png, dpi=160)
    plt.close(fig)


DETAIL_SPAN_HZ = 900e3  # half-width of the detail view; --span-khz overrides


def detail_span(p, rate, center):
    """Half-width of the detail view around the signal: wide context like an SDR waterfall
    (+-900 kHz by default), limited by what the capture actually contains on both sides."""
    f_off = p["freq"] - center
    room = rate / 2 - abs(f_off) - 20e3
    return max(min(DETAIL_SPAN_HZ, room), occupied_bw(p) * 0.75)


def draw_detail(raw, skip, rate, center, p, pkt, meta, out_png):
    from matplotlib.ticker import MultipleLocator

    nper = detail_nperseg(p, rate)
    nper_zoom = detail_nperseg(p, rate, zoom=True)
    pad = max(0.06 * pkt["dur"], 0.002)
    s0 = max(0, int((pkt["start"] - pad) * rate))
    s1 = min(raw.size // 2 - skip, int((pkt["end"] + pad) * rate))
    x = iq_from_bytes(raw[2 * (skip + s0): 2 * (skip + s1)])

    f_off = p["freq"] - center
    half_span = detail_span(p, rate, center)
    dc_k = -f_off / 1e3  # tuner DC spike, relative to the signal centre

    # zoom window: LoRa preamble (8 up-chirps) + sync word + 2.25 down-chirps + a few payload symbols
    if p["kind"] == "lora":
        zoom_s = 16 * (2 ** p["sf"]) / p["bw"]
        zoom_label = "початок пакета: преамбула (up-chirps), sync word, down-chirps"
    else:
        zoom_s = 72 / p["bitrate"]
        zoom_label = "початок пакета: преамбула 0101… і sync word (перемикання двох тонів)"
    start = refine_start(x, rate, p, pad)
    t_zero = (s0 + start) / rate  # 0 ms on the time axes = sample-accurate packet start
    z0 = max(0, start - int(0.08 * zoom_s * rate))
    z1 = min(len(x), z0 + int(zoom_s * 1.15 * rate))
    xz = x[z0:z1]

    # Time runs left -> right, frequency bottom -> top, offsets in kHz from the signal centre,
    # 100 kHz grid - the classic SDR-waterfall layout.
    fig, (a1, a2) = plt.subplots(2, 1, figsize=(16, 12))
    for ax, sig, nps, title, t_origin in (
            (a1, x, nper, f"увесь пакет ({pkt['dur'] * 1e3:.1f} мс)", s0 / rate),
            (a2, xz, nper_zoom, zoom_label, (s0 + z0) / rate)):
        f, t, S = stft_db(sig, rate, nps, max_cols=2600, overlap_div=16 if ax is a2 else 8, nfft=1024)
        rel = (f - f_off)
        keep = (rel >= -half_span) & (rel <= half_span)
        rel_k, S = rel[keep] / 1e3, S[keep]
        vmin, vmax = robust_limits(S, 40, 99.97)
        t_ms = (t + t_origin - t_zero) * 1e3
        im = ax.imshow(S, aspect="auto", cmap="inferno", vmin=vmin, vmax=vmax, origin="lower",
                       extent=[t_ms[0], t_ms[-1], rel_k[0], rel_k[-1]], interpolation="bilinear")
        ax.yaxis.set_major_locator(MultipleLocator(100 if half_span > 250e3 else 25))
        ax.yaxis.set_minor_locator(MultipleLocator(20 if half_span > 250e3 else 5))
        ax.set_ylabel("Зсув від центру сигналу, кГц")
        ax.set_title(f"{title}   [STFT {nps} точок ≈ {rate / nps / 1e3:.1f} кГц × {nps / rate * 1e6:.0f} мкс]",
                     fontsize=10)
        bw_k = occupied_bw(p) / 2e3
        for yy in (-bw_k, bw_k):
            ax.axhline(yy, color="cyan", ls="--", lw=0.7, alpha=0.55)
        if -half_span / 1e3 < dc_k < half_span / 1e3:
            ax.text(t_ms[-1], dc_k, "DC тюнера ", color="violet", ha="right", va="bottom", fontsize=8, alpha=0.9)
        fig.colorbar(im, ax=ax, label="дБ", pad=0.01)
    a1.set_xlabel("Час від початку пакета, мс")
    a2.set_xlabel("Час від початку пакета, мс")
    fig.suptitle(f"{p['title']} — детальний знімок пакета\n"
                 f"центр сигналу {p['freq'] / 1e6:.3f} МГц · {meta['timestamp']} · gain {meta['gain']} дБ · "
                 f"SNR ≈ {meta['snr_db']:.0f} дБ · {rate / 1e6:.1f} MS/s   (блакитні лінії — смуга сигналу)",
                 fontsize=11)
    fig.tight_layout()
    fig.savefig(out_png, dpi=150)
    plt.close(fig)


# ---------------------------------------------------------------------------
# Top level
# ---------------------------------------------------------------------------

def analyze(iq_path, p, center, rate, gain, out_base, timestamp):
    raw = np.memmap(iq_path, dtype=np.uint8, mode="r")
    if raw.size < 4:
        raise RuntimeError("Порожній запис")
    skip = int(0.1 * rate)  # the first ~100 ms after start-up are tuner settling transients
    clip = float(np.count_nonzero((raw == 0) | (raw == 255)) / raw.size * 100)

    ov = overview(raw, rate, center, p, skip)
    floor, segs = find_packets(ov["band_db"], ov["frame_s"], p)
    pkt, matched = pick_packet(segs, p)
    meta = dict(profile=p["label"], title=p["title"], timestamp=timestamp, center_hz=center,
                signal_hz=p["freq"], rate=rate, gain=gain, clipping_percent=round(clip, 4),
                noise_floor_db=round(floor, 1), segments_found=len(segs),
                expected_airtime_ms=p["airtime"] * 1e3, snr_db=0.0)
    if pkt:
        meta.update(snr_db=round(pkt["mean_db"] - floor, 1), packet_start_s=round(pkt["start"], 4),
                    packet_ms=round(pkt["dur"] * 1e3, 1), packet_matches_expected=matched)

    files = dict(overview=out_base + "_overview.png")
    draw_overview(ov, center, rate, p, pkt, meta, files["overview"])
    if pkt:
        files["detail"] = out_base + "_detail.png"
        draw_detail(raw, skip, rate, center, p, pkt, meta, files["detail"])
    meta["files"] = files
    with open(out_base + "_meta.json", "w", encoding="utf-8") as fh:
        json.dump(meta, fh, ensure_ascii=False, indent=2)
    del raw
    return meta


def report(meta):
    print(f"  Знайдено сегментів у смузі: {meta['segments_found']}")
    if "packet_ms" in meta:
        tag = "" if meta["packet_matches_expected"] else "  (УВАГА: тривалість не схожа на очікувану)"
        print(f"  Пакет: {meta['packet_ms']} мс (очікувано ≈{meta['expected_airtime_ms']:.0f} мс), "
              f"SNR ≈ {meta['snr_db']} дБ{tag}")
    else:
        print("  Пакет НЕ знайдено - перевірте, що пристрої в TEST на цьому профілі і Base передає.")
    if meta["clipping_percent"] > 0.05:
        print(f"  УВАГА: перевантаження АЦП ({meta['clipping_percent']:.2f}% відліків у межі) - "
              f"зменште gain або відсуньте пристрої від SDR, інакше на знімку будуть паразитні лінії.")
    elif "snr_db" in meta and 0 < meta["snr_db"] < 15:
        print("  Порада: сигнал слабкий для гарного знімка - збільште gain.")
    for k, v in meta["files"].items():
        print(f"  {k}: {v}")


def run_one(key, gain, outdir, rate=DEFAULT_RATE, device=0, ppm=0, keep_iq=False, duration=None):
    p = PROFILES[key]
    center = p["freq"] + tuning_offset(p)
    ts = datetime.datetime.now()
    day_dir = os.path.join(outdir, ts.strftime("%Y%m%d"))
    os.makedirs(day_dir, exist_ok=True)
    base = os.path.join(day_dir, f"{p['label']}_{ts.strftime('%H%M%S')}")
    iq = base + "_iq.cu8"
    capture(center, rate, duration or p["duration"], gain, iq, device, ppm)
    print("Обробка…")
    meta = analyze(iq, p, center, rate, gain, base, ts.strftime("%Y-%m-%d %H:%M:%S"))
    if keep_iq:
        meta["files"]["iq"] = iq
    else:
        os.remove(iq)
    return meta


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--profile", required=True, choices=list(PROFILES), help="1..6 або S (службовий канал)")
    ap.add_argument("--gain", type=float, default=0.0,
                    help="посилення тюнера, дБ (0..49.6); 0 — мінімум, для пристроїв поруч із SDR")
    ap.add_argument("--rate", type=float, default=DEFAULT_RATE)
    ap.add_argument("--duration", type=float, default=None, help="тривалість запису, с (типово — за профілем)")
    ap.add_argument("--device", type=int, default=0)
    ap.add_argument("--ppm", type=int, default=0)
    ap.add_argument("--outdir", default="captures")
    ap.add_argument("--keep-iq", action="store_true", help="зберегти сирий .cu8")
    ap.add_argument("--span-khz", type=float, default=900, help="півширина детального знімка по частоті, кГц")
    ap.add_argument("--analyze", metavar="CU8", help="лише обробити наявний запис (центр = частота + зміщення)")
    args = ap.parse_args()

    global DETAIL_SPAN_HZ
    DETAIL_SPAN_HZ = args.span_khz * 1e3
    p = PROFILES[args.profile]
    if args.analyze:
        base = os.path.splitext(args.analyze)[0].removesuffix("_iq")
        meta = analyze(args.analyze, p, p["freq"] + tuning_offset(p), args.rate, args.gain, base,
                       datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
    else:
        meta = run_one(args.profile, args.gain, args.outdir, args.rate, args.device, args.ppm,
                       args.keep_iq, args.duration)
    report(meta)


if __name__ == "__main__":
    sys.exit(main())
