#!/usr/bin/env python3
"""Test helper: taps a key through a virtual uinput keyboard, so a game reaches its world without a person.
usage: press_keys.py [--delay 3] [--count 10] [--every 0.5] [--key KEY_E]"""
import argparse, time
from evdev import UInput, ecodes as e

ap = argparse.ArgumentParser()
ap.add_argument("--delay", type=float, default=3)
ap.add_argument("--count", type=int, default=10)
ap.add_argument("--every", type=float, default=0.5)
ap.add_argument("--key", default="KEY_E")
a = ap.parse_args()
key = getattr(e, a.key)
with UInput({e.EV_KEY: [key]}, name="dlssg-sm86 test keys") as ui:
    time.sleep(a.delay)
    for _ in range(a.count):
        ui.write(e.EV_KEY, key, 1); ui.syn(); time.sleep(0.08)
        ui.write(e.EV_KEY, key, 0); ui.syn(); time.sleep(a.every)
