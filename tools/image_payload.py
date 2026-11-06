#!/usr/bin/env python3
"""Build an image/set payload from text frames.

A frame is a text block, one line per matrix row, one character per letter
(spaces are ignored). Characters map to colours via --palette; '.' is off.
An optional extra line starting with '+' gives the minute LEDs 1..4.
Frames in one file are separated by a blank line.

    python3 tools/image_payload.py heart.txt --palette r=FF3B3B --duration 10
    python3 tools/image_payload.py fire.txt --palette r=FF3B3B,o=FF8A1F \\
        --ms 150 --loops 3 | mosquitto_pub -h broker -t wortuhr/image/set -s

Prints the JSON payload; nothing is sent by this script.
"""

from __future__ import annotations

import argparse
import json
import sys


def parse_palette(text: str) -> dict[str, str]:
    palette = {".": "000000"}
    for entry in filter(None, text.split(",")):
        key, _, value = entry.partition("=")
        value = value.lstrip("#").upper()
        if len(key) != 1 or len(value) != 6:
            raise SystemExit(f"bad palette entry {entry!r}, expected x=RRGGBB")
        int(value, 16)
        palette[key] = value
    return palette


def parse_frames(text: str, rows: int, cols: int) -> list[tuple[list[str], str]]:
    frames = []
    for block in text.strip().split("\n\n"):
        lines = [line.replace(" ", "") for line in block.strip().splitlines()]
        corners = ""
        if lines and lines[-1].startswith("+"):
            corners = lines.pop()[1:]
            if len(corners) != 4:
                raise SystemExit("the '+' line needs exactly 4 characters")
        if len(lines) != rows or any(len(line) != cols for line in lines):
            raise SystemExit(f"each frame needs {rows} lines of {cols} characters")
        frames.append((lines, corners))
    return frames


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("file", help="text frames, '-' for stdin")
    parser.add_argument("--palette", default="", help="x=RRGGBB,y=RRGGBB,...")
    parser.add_argument("--rows", type=int, default=10)
    parser.add_argument("--cols", type=int, default=11)
    parser.add_argument("--ms", type=int, default=0, help="time per frame")
    parser.add_argument("--loops", type=int, default=0, help="0 = endless")
    parser.add_argument("--duration", type=int, default=0, help="seconds, 0 = no limit")
    parser.add_argument("--brightness", type=int, help="percent, default: clock brightness")
    args = parser.parse_args()

    text = sys.stdin.read() if args.file == "-" else open(args.file, encoding="utf-8").read()
    palette = parse_palette(args.palette)

    frames = []
    for lines, corners in parse_frames(text, args.rows, args.cols):
        try:
            pixels = "".join(palette[c] for c in "".join(lines) + corners)
        except KeyError as missing:
            raise SystemExit(f"colour {missing} is not in the palette")
        frames.append({"pixels": pixels, "ms": args.ms})

    payload = {"frames": frames, "loops": args.loops, "duration": args.duration}
    if args.brightness is not None:
        payload["brightness"] = args.brightness
    print(json.dumps(payload, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
