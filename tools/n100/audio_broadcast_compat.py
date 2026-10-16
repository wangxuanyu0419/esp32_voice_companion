"""Bounded sender-side compatibility preparation for ESP32 audio broadcast.

The ClawChat server intentionally serves the source file unchanged.  The
ESP32 simple M4A decoder accepts a regular (flat/faststart) MP4 container but
not fragmented MP4 (``mvex``/``moof`` with an empty ``stsc`` table).  This
module performs a lossless copy-remux only when a broadcast can reach the
ESP32.  Non-fragmented files are passed through unchanged.

The caller owns the HTTP POST.  ``prepared_audio_for_broadcast`` yields a
spooled path retained for the server's stream lifetime; the next invocation
performs bounded expiry cleanup.  A remux failure is a hard error: callers
must not fall back to sending the incompatible source to an ESP32 target.
"""
from __future__ import annotations

import contextlib
import fcntl
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Iterator


ESP32_DEVICE_ID = "esp32s3_001"

# Keep the conversion bounded for a periodic sender.  These limits are well
# above the existing NAS tracks but prevent an accidental multi-gigabyte or
# hour-long source from monopolising the sender.
MAX_INPUT_BYTES = 64 * 1024 * 1024
MAX_DURATION_SECONDS = 15 * 60
PROBE_BYTES = 1024 * 1024
FFPROBE_TIMEOUT_SECONDS = 5
FFMPEG_TIMEOUT_SECONDS = 60
SPOOL_DIR = Path(
    os.environ.get(
        "CLAWCHAT_ESP32_AUDIO_SPOOL_DIR",
        str(Path.home() / ".hermes/cache/clawchat-esp32-audio-remux"),
    )
)
LOCK_PATH = SPOOL_DIR / "clawchat-esp32-audio-remux.lock"
STREAM_TTL_DEFAULT_SECONDS = 6 * 60 * 60
STREAM_TTL_GRACE_SECONDS = 5 * 60
MAX_SPOOL_FILES = 16
MAX_SPOOL_BYTES = 128 * 1024 * 1024


class AudioCompatibilityError(RuntimeError):
    """The source cannot be safely prepared for the selected target."""


def _should_prepare_for_esp(device_id: str | None) -> bool:
    # An omitted device_id means the server broadcasts to all connected
    # devices.  Remuxing fMP4 to an ordinary MP4 is transparent to Android's
    # ExoPlayer, so the all-device path must also prepare it; otherwise the
    # ESP32 would continue receiving the known-incompatible source.
    return not device_id or device_id == ESP32_DEVICE_ID


def _stream_ttl_seconds() -> int:
    """Read the deployed server TTL without exposing any other .env value."""
    raw = os.environ.get("AUDIO_BROADCAST_TTL_MS", "").strip()
    if not raw:
        server_env = Path("/home/xuanyu/clawchat-companion/server/.env")
        try:
            for line in server_env.read_text(encoding="utf-8").splitlines():
                if line.strip().startswith("AUDIO_BROADCAST_TTL_MS="):
                    raw = line.split("=", 1)[1].strip().strip('"').strip("'")
                    break
        except OSError:
            pass
    try:
        milliseconds = int(raw)
        if milliseconds > 0:
            return max(60, (milliseconds + 999) // 1000)
    except (TypeError, ValueError):
        pass
    return STREAM_TTL_DEFAULT_SECONDS


def _is_spool_file(path: Path) -> bool:
    return path.name.startswith(".clawchat-esp32-") and path.suffix == ".m4a"


def _cleanup_expired_spool(now: float | None = None) -> None:
    """Remove only remux outputs older than the server stream lifetime."""
    now = time.time() if now is None else now
    try:
        entries = list(SPOOL_DIR.iterdir())
    except OSError:
        return
    retention = _stream_ttl_seconds() + STREAM_TTL_GRACE_SECONDS
    for path in entries:
        if not _is_spool_file(path):
            # A stale interrupted ffmpeg output is safe to remove after one
            # hour; it was never registered with the server.
            if path.name.startswith(".clawchat-esp32-") and path.suffix == ".partial":
                try:
                    if now - path.stat().st_mtime > 60 * 60:
                        path.unlink(missing_ok=True)
                except OSError:
                    pass
            continue
        try:
            if now - path.stat().st_mtime >= retention:
                path.unlink(missing_ok=True)
        except OSError:
            pass


def _ensure_spool_capacity(required_bytes: int) -> None:
    _cleanup_expired_spool()
    try:
        files = [p for p in SPOOL_DIR.iterdir() if _is_spool_file(p)]
        used = sum(p.stat().st_size for p in files)
    except OSError as exc:
        raise AudioCompatibilityError("cannot inspect remux spool") from exc
    if len(files) >= MAX_SPOOL_FILES or used + required_bytes > MAX_SPOOL_BYTES:
        raise AudioCompatibilityError("remux spool is full; refusing early eviction")


def _validate_source(path: Path, *, strict: bool) -> Path:
    try:
        resolved = path.expanduser().resolve(strict=True)
        stat = resolved.stat()
    except (OSError, RuntimeError) as exc:
        raise AudioCompatibilityError(f"audio source unavailable: {path}") from exc
    if not resolved.is_file() or stat.st_size <= 0:
        raise AudioCompatibilityError(f"audio source is not a regular file: {path}")
    if strict and stat.st_size > MAX_INPUT_BYTES:
        raise AudioCompatibilityError(
            f"audio source exceeds {MAX_INPUT_BYTES} byte safety limit"
        )
    return resolved


def _probe_window(path: Path) -> bytes:
    with path.open("rb") as source:
        head = source.read(PROBE_BYTES)
        if path.stat().st_size <= PROBE_BYTES:
            return head
        source.seek(-PROBE_BYTES, os.SEEK_END)
        tail = source.read(PROBE_BYTES)
    return head + tail


def is_fragmented_mp4(path: Path) -> bool:
    """Return true for an MP4/M4A carrying fragmented-container markers."""
    if path.suffix.lower() not in {".m4a", ".mp4"}:
        return False
    try:
        probe = _probe_window(path)
    except OSError as exc:
        raise AudioCompatibilityError(f"cannot inspect audio source: {path}") from exc
    # The markers are ISO-BMFF atom names.  Looking in bounded head+tail
    # windows avoids loading media data; normal flat files have neither marker.
    return b"mvex" in probe or b"moof" in probe


def _duration_seconds(path: Path) -> float | None:
    ffprobe = shutil.which("ffprobe")
    if not ffprobe:
        raise AudioCompatibilityError("ffprobe is required for bounded remux")
    try:
        result = subprocess.run(
            [
                ffprobe,
                "-v",
                "error",
                "-select_streams",
                "a:0",
                "-show_entries",
                "format=duration",
                "-of",
                "default=noprint_wrappers=1:nokey=1",
                str(path),
            ],
            capture_output=True,
            text=True,
            timeout=FFPROBE_TIMEOUT_SECONDS,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise AudioCompatibilityError("ffprobe timed out or is unavailable") from exc
    if result.returncode != 0:
        raise AudioCompatibilityError("ffprobe rejected audio source")
    try:
        duration = float(result.stdout.strip())
    except (TypeError, ValueError):
        raise AudioCompatibilityError("audio duration is unavailable")
    if duration <= 0 or duration > MAX_DURATION_SECONDS:
        raise AudioCompatibilityError(
            f"audio duration outside {MAX_DURATION_SECONDS}s safety limit"
        )
    return duration


@contextlib.contextmanager
def _exclusive_remux_lock() -> Iterator[None]:
    SPOOL_DIR.mkdir(parents=True, exist_ok=True, mode=0o700)
    try:
        os.chmod(SPOOL_DIR, 0o700)
    except OSError:
        pass
    fd = os.open(LOCK_PATH, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        with os.fdopen(fd, "rb", closefd=True) as lock:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
            yield
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
    finally:
        # The lock file is intentionally retained: concurrent processes can
        # safely reopen the same inode and it contains no user data.
        pass


def _remux_flat(source: Path) -> Path:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise AudioCompatibilityError("ffmpeg is required for fMP4 remux")
    _duration_seconds(source)
    with _exclusive_remux_lock():
        _ensure_spool_capacity(source.stat().st_size)
        fd, name = tempfile.mkstemp(
            prefix=".clawchat-esp32-", suffix=".partial", dir=SPOOL_DIR
        )
        os.close(fd)
        partial = Path(name)
        try:
            try:
                result = subprocess.run(
                    [
                        ffmpeg,
                        "-hide_banner",
                        "-loglevel",
                        "error",
                        "-y",
                        "-i",
                        str(source),
                        "-map",
                        "0:a:0",
                        "-c:a",
                        "copy",
                        "-movflags",
                        "+faststart",
                        "-vn",
                        "-f",
                        "mp4",
                        str(partial),
                    ],
                    capture_output=True,
                    text=True,
                    timeout=FFMPEG_TIMEOUT_SECONDS,
                    check=False,
                )
            except (OSError, subprocess.TimeoutExpired) as exc:
                raise AudioCompatibilityError("ffmpeg remux timed out or failed") from exc
            if result.returncode != 0 or not partial.is_file():
                detail = (result.stderr or "").strip()[:200]
                raise AudioCompatibilityError(
                    f"ffmpeg remux failed{': ' + detail if detail else ''}"
                )
            if partial.stat().st_size <= 0 or partial.stat().st_size > MAX_INPUT_BYTES:
                raise AudioCompatibilityError("remuxed output exceeds safety limit")
            output = partial.with_suffix(".m4a")
            os.replace(partial, output)
            try:
                # A successful copy-remux must no longer be fragmented.  This
                # catches a surprising ffmpeg build/configuration before
                # anything is registered with the server.
                if is_fragmented_mp4(output):
                    raise AudioCompatibilityError("ffmpeg output is still fragmented MP4")
                return output
            except Exception:
                output.unlink(missing_ok=True)
                raise
        except Exception:
            partial.unlink(missing_ok=True)
            raise


@contextlib.contextmanager
def prepared_audio_for_broadcast(
    file_path: str | os.PathLike[str],
    *,
    device_id: str | None = None,
) -> Iterator[Path]:
    """Yield a safe source path for one audio-broadcast request.

    For an explicit non-ESP target, the existing source is preserved exactly.
    For ESP32 or all-device broadcasts, fMP4 is losslessly remuxed; a failed
    remux raises before the caller can POST the original bytes.
    """
    prepare = _should_prepare_for_esp(device_id)
    source = _validate_source(Path(file_path), strict=prepare)
    if not prepare or not is_fragmented_mp4(source):
        yield source
        return

    # The server registers the path and only opens it when the WS recipient
    # fetches /audio-stream/<id>.  Keep this file for the actual server TTL;
    # the next sender invocation performs bounded expiry cleanup.  Deleting
    # it when POST returns would race the device fetch and produce HTTP 404.
    yield _remux_flat(source)
