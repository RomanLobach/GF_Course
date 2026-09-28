#!/usr/bin/env python3
"""Guided waterfall session: walks through the practicum's radio profiles one by one while
the two devices run in TEST mode, capturing and rendering each.

    ~/radioconda/bin/python run_all.py                 # all profiles, gain 0 dB
    ~/radioconda/bin/python run_all.py --gain 20 --profiles 3,4,S --open

Per profile: set the profile on the devices, press Enter, the script records and renders
(see capture_waterfall.py), prints what it found, then asks what to do next.
"""
import argparse
import subprocess
import sys

import capture_waterfall
from capture_waterfall import PROFILES, DEFAULT_RATE, run_one, report

HOW_TO = {
    "S": "Службовий канал видно в будь-якому режимі з обміном: у TEST (слоти після кожної пачки) "
         "або в SYNC (хардбіт кожні 5 с по черзі). Запис довший, щоб піймати кілька пакетів.",
}


def ask(prompt):
    try:
        return input(prompt).strip()
    except EOFError:
        return "q"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--profiles", default="1,2,3,4,5,6,S", help="порядок і перелік, напр. 3,4,S")
    ap.add_argument("--gain", type=float, default=0.0, help="0 дБ для пристроїв поруч; далі — більше")
    ap.add_argument("--rate", type=float, default=DEFAULT_RATE)
    ap.add_argument("--device", type=int, default=0)
    ap.add_argument("--ppm", type=int, default=0)
    ap.add_argument("--outdir", default="captures")
    ap.add_argument("--keep-iq", action="store_true")
    ap.add_argument("--open", action="store_true", help="відкривати готові PNG (macOS)")
    ap.add_argument("--span-khz", type=float, default=900, help="півширина детального знімка по частоті, кГц")
    args = ap.parse_args()
    capture_waterfall.DETAIL_SPAN_HZ = args.span_khz * 1e3

    keys = [k.strip().upper() for k in args.profiles.split(",") if k.strip()]
    bad = [k for k in keys if k not in PROFILES]
    if bad:
        sys.exit(f"Невідомі профілі: {bad}; доступні: {list(PROFILES)}")

    gain = args.gain
    print("Обидва пристрої мають бути в TEST («Тест частот [почати]»). Профіль обирається обертанням\n"
          "енкодера на головному екрані будь-якого пристрою; поки перемикання не завершилось — у\n"
          "статус-барі блимає крапка. Записуйте, коли крапка зникла.\n"
          "Посилення: 0 дБ, якщо пристрої на столі поруч із SDR (навіть 8 дБ уже перевантажують АЦП);\n"
          "якщо SNR < 15 дБ — збільшуйте (g 10, g 20…). Про перевантаження скрипт попередить сам.\n")
    i = 0
    while i < len(keys):
        key = keys[i]
        p = PROFILES[key]
        print(f"\n=== {p['title']}  ({p['freq'] / 1e6:.3f} МГц) ===")
        if key in HOW_TO:
            print(HOW_TO[key])
        else:
            print(f"Оберіть на пристрої профіль {key}.")
        cmd = ask(f"Enter — запис {p['duration']:.0f} с (gain {gain} дБ), s — пропустити, q — вийти: ")
        if cmd.lower() == "q":
            break
        if cmd.lower() == "s":
            i += 1
            continue

        while True:
            try:
                meta = run_one(key, gain, args.outdir, args.rate, args.device, args.ppm, args.keep_iq)
                report(meta)
                if args.open:
                    subprocess.run(["open"] + [v for k, v in meta["files"].items() if v.endswith(".png")])
            except Exception as exc:  # keep the session alive on a single failed capture
                print("Помилка:", exc)
            nxt = ask("Enter — далі, r — повторити, g <дБ> — повторити з іншим gain, q — вийти: ")
            if nxt.lower().startswith("g"):
                try:
                    gain = float(nxt[1:].strip())
                except ValueError:
                    print("Формат: g 20")
                continue
            if nxt.lower() == "r":
                continue
            if nxt.lower() == "q":
                i = len(keys)
            break
        i += 1
    print("\nГотово. Знімки в", args.outdir)


if __name__ == "__main__":
    main()
