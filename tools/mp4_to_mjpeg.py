#!/usr/bin/env python3
"""Convert an MP4 into ESP32-friendly MJPEG video plus optional WAV audio.

The generated files are intended for the Waveshare ESP32-S3 AMOLED player:
  VIDEO.MJPG  concatenated JPEG frames, fixed 368x410 by default
  AUDIO.WAV   PCM16 mono 16 kHz, matching the current ES8311/I2S setup

By default the video is center-cropped to fill the target size and the output
directory name is shortened for ESP32 FATFS builds without long filename support.

Requires ffmpeg on PATH, for example: brew install ffmpeg
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
from pathlib import Path


DEFAULT_WIDTH = 368
DEFAULT_HEIGHT = 410
DEFAULT_FPS = 10
DEFAULT_JPEG_QUALITY = 7
DEFAULT_AUDIO_RATE = 16000


def run(cmd: list[str]) -> None:
    print("+ " + " ".join(cmd))
    subprocess.run(cmd, check=True)


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if not path:
        raise SystemExit(f"{name} not found. Install it with: brew install ffmpeg")
    return path


def default_out_dir(src: Path) -> Path:
    safe = "".join(ch for ch in src.stem.upper() if ch.isalnum())
    digits = "".join(re.findall(r"\d+", safe))
    if digits:
        name = f"VID{digits[-5:]}"[:8]
    else:
        name = safe[:8] if safe else "VIDEO"
    return src.with_name(name)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert MP4 to fixed-size MJPEG + WAV for ESP32 playback."
    )
    parser.add_argument("input", type=Path, help="Input .mp4 file")
    parser.add_argument(
        "-o",
        "--out-dir",
        type=Path,
        help="Output directory. Defaults to an ESP32-friendly short name next to input.",
    )
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    parser.add_argument("--fps", type=int, default=DEFAULT_FPS)
    parser.add_argument(
        "--quality",
        type=int,
        default=DEFAULT_JPEG_QUALITY,
        help="ffmpeg MJPEG q:v value, lower is better/larger. Recommended 6-9.",
    )
    parser.add_argument(
        "--no-audio",
        action="store_true",
        help="Only generate VIDEO.MJPG, skip AUDIO.WAV.",
    )
    parser.add_argument(
        "--audio-rate",
        type=int,
        default=DEFAULT_AUDIO_RATE,
        help="WAV sample rate. Keep 16000 for the current firmware.",
    )
    parser.add_argument(
        "--cover",
        action="store_true",
        default=True,
        help="Fill the screen and crop edges. This is the default.",
    )
    parser.add_argument(
        "--contain",
        dest="cover",
        action="store_false",
        help="Preserve the full frame and letterbox instead of cropping.",
    )
    parser.add_argument(
        "--max-error-rate",
        default="1.0",
        help="ffmpeg decode error threshold. 1.0 keeps best-effort output for damaged MP4s.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    src = args.input.expanduser().resolve()
    if not src.exists():
        raise SystemExit(f"Input not found: {src}")
    if not src.is_file():
        raise SystemExit(f"Input is not a file: {src}")

    ffmpeg = require_tool("ffmpeg")

    out_dir = args.out_dir.expanduser().resolve() if args.out_dir else default_out_dir(src)
    out_dir.mkdir(parents=True, exist_ok=True)

    video_out = out_dir / "VIDEO.MJPG"
    audio_out = out_dir / "AUDIO.WAV"

    if args.cover:
        vf = (
            f"fps={args.fps},"
            f"scale={args.width}:{args.height}:force_original_aspect_ratio=increase,"
            f"crop={args.width}:{args.height},"
            "format=yuvj420p"
        )
    else:
        vf = (
            f"fps={args.fps},"
            f"scale={args.width}:{args.height}:force_original_aspect_ratio=decrease,"
            f"pad={args.width}:{args.height}:(ow-iw)/2:(oh-ih)/2:color=black,"
            "format=yuvj420p"
        )

    run(
        [
            ffmpeg,
            "-y",
            "-max_error_rate",
            args.max_error_rate,
            "-i",
            str(src),
            "-vf",
            vf,
            "-an",
            "-c:v",
            "mjpeg",
            "-q:v",
            str(args.quality),
            "-f",
            "mjpeg",
            str(video_out),
        ]
    )

    if not args.no_audio:
        run(
            [
                ffmpeg,
                "-y",
                "-max_error_rate",
                args.max_error_rate,
                "-i",
                str(src),
                "-vn",
                "-ac",
                "1",
                "-ar",
                str(args.audio_rate),
                "-sample_fmt",
                "s16",
                str(audio_out),
            ]
        )

    print()
    print(f"Wrote: {out_dir}")
    print(f"  {video_out.name}")
    if not args.no_audio:
        print(f"  {audio_out.name}")
    print()
    print("Copy the output directory to the TF card, for example:")
    print(f"  /sdcard/media/{out_dir.name}/VIDEO.MJPG")
    if not args.no_audio:
        print(f"  /sdcard/media/{out_dir.name}/AUDIO.WAV")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        raise SystemExit(exc.returncode)
