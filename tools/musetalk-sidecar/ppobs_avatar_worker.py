#!/usr/bin/env python3
"""Avatar lip-sync worker for ppobs.

The plugin (plugins/obs-avatar) owns the named pipe and this process connects
to it, which is why nothing here needs pywin32: connecting to a Windows named
pipe is just open(r"\\\\.\\pipe\\name", "r+b"), while *creating* one is not.

Protocol: plugins/obs-avatar/avatar-sidecar-protocol.h is the authority. Every
integer is little-endian, every message is an 8-byte header (uint32 payload
length, uint16 type, uint16 reserved-must-be-zero) followed by its payload.
The byte fixtures in --selftest are the same ones avatar-protocol-test.c pins,
so the two languages disagreeing shows up as a failing test rather than a
garbled frame on stream.

Two engines:

  StubEngine      no model, no dependencies beyond the standard library. Draws
                  a mouth bar whose opening follows the audio level, so the
                  whole path - tap, resample, pipe, frame, upload - can be
                  verified, and its latency seen, before MuseTalk weights
                  exist.

  MuseTalkEngine  the real thing. Not implemented yet; it reports what is
                  missing rather than pretending, and the worker falls back to
                  the stub so the pipe still comes up.

Usage:
    python ppobs_avatar_worker.py --pipe \\\\.\\pipe\\ppobs-avatar-1234-0000
    python ppobs_avatar_worker.py --selftest
"""

from __future__ import annotations

import argparse
import math
import os
import struct
import sys
import time

# --------------------------------------------------------------------------
# Protocol (keep in step with avatar-sidecar-protocol.h)
# --------------------------------------------------------------------------

PROTOCOL_VERSION = 1

HEADER = struct.Struct("<IHH")
HEADER_SIZE = 8
MAX_PAYLOAD = 4 * 1024 * 1024 + 64
FRAME_MAX_EDGE = 1024

MSG_HELLO = 1
MSG_READY = 2
MSG_AUDIO = 3
MSG_FRAME = 4
MSG_STAT = 5
MSG_LOG = 6
MSG_ERROR = 7
MSG_BYE = 8

READY_REAL_MODEL = 1 << 0

# OBS's own log level numbers, so neither side needs a mapping table.
LOG_ERROR = 100
LOG_WARNING = 200
LOG_INFO = 300
LOG_DEBUG = 400

AUDIO_HEADER = struct.Struct("<qI")  # pts_ns, sample_count
FRAME_HEADER = struct.Struct("<qII")  # pts_ns, width, height
STAT_PAYLOAD = struct.Struct("<IIIII")  # whisper_us, unet_us, vae_us, queue, dropped
HELLO_PAYLOAD = struct.Struct("<IIII")  # version, sample_rate, fps, asset_len


class ProtocolError(Exception):
    pass


def read_exact(stream, count: int) -> bytes:
    """A pipe read may return any prefix of what was asked for; that partial
    case is exactly what the length framing exists to handle, so never trust a
    single read."""
    chunks = []
    remaining = count
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            raise ProtocolError("pipe closed")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_message(stream):
    payload_len, msg_type, reserved = HEADER.unpack(read_exact(stream, HEADER_SIZE))
    if reserved != 0:
        raise ProtocolError(f"reserved field set ({reserved})")
    if payload_len > MAX_PAYLOAD:
        raise ProtocolError(f"payload too large ({payload_len})")
    payload = read_exact(stream, payload_len) if payload_len else b""
    return msg_type, payload


def send_message(stream, msg_type: int, payload: bytes = b"") -> None:
    # Header and payload go out as two writes rather than one concatenation:
    # a frame payload is a quarter of a megabyte and copying it to prepend
    # eight bytes is pure waste. Only one thread ever writes, so the two
    # writes cannot interleave with another message.
    stream.write(HEADER.pack(len(payload), msg_type, 0))
    if payload:
        stream.write(payload)
    stream.flush()


def send_log(stream, level: int, text: str) -> None:
    send_message(stream, MSG_LOG, struct.pack("<I", level) + text.encode("utf-8"))


def send_error(stream, code: int, text: str) -> None:
    send_message(stream, MSG_ERROR, struct.pack("<I", code) + text.encode("utf-8"))


# --------------------------------------------------------------------------
# Engines
# --------------------------------------------------------------------------


class StubEngine:
    """No model. Renders a face-shaped placeholder whose mouth opens with the
    audio level.

    The point is not to look good - it is that every link in the chain can be
    checked without a 3.4 GB download: the tap gets audio, the resampler makes
    16 kHz mono, the pipe carries it, frames come back, and the source puts
    them on screen. Watching the mouth bar react to speech also shows the
    end-to-end latency directly, which is the number stage D has to
    compensate."""

    name = "stub (no model)"
    real_model = False

    BACKGROUND = bytes((24, 22, 20, 255))
    FACE = bytes((150, 180, 210, 255))
    MOUTH = bytes((20, 20, 90, 255))

    def __init__(self, width: int = 256, height: int = 256) -> None:
        self.width = width
        self.height = height

        # The face is drawn once. A per-pixel Python loop over 256x256 costs
        # well over a frame's worth of time at 25 fps, which would make the
        # stub itself the bottleneck and the measured latency meaningless -
        # so per frame all that happens is a buffer copy plus one slice
        # assignment per mouth row.
        cx, cy = width / 2.0, height / 2.0
        head_rx, head_ry = width * 0.34, height * 0.42

        base = bytearray()
        for y in range(height):
            dy = (y - cy) / head_ry
            inner = 1.0 - dy * dy
            if inner <= 0.0:
                base += self.BACKGROUND * width
                continue

            half = head_rx * math.sqrt(inner)
            left = max(0, int(cx - half))
            right = min(width, int(cx + half))
            base += self.BACKGROUND * left
            base += self.FACE * (right - left)
            base += self.BACKGROUND * (width - right)

        self._base = bytes(base)
        self._mouth_cy = cy + height * 0.18
        self._mouth_half_w = width * 0.16
        self._mouth_max_half = max(2.0, height * 0.09)

    def render(self, rms: float, peak: float) -> bytes:
        # dBFS, floored the way the plugin floors it, then mapped from the
        # plugin's silence threshold (-50) up to -6 onto shut..open.
        level_db = 20.0 * math.log10(peak) if peak > 1e-6 else -120.0
        openness = max(0.0, min(1.0, (level_db + 50.0) / 44.0))

        frame = bytearray(self._base)

        half = self._mouth_max_half * openness
        if half >= 1.0:
            w = self.width
            left = max(0, int(self.width / 2.0 - self._mouth_half_w))
            right = min(w, int(self.width / 2.0 + self._mouth_half_w))
            row_fill = self.MOUTH * (right - left)

            top = max(0, int(self._mouth_cy - half))
            bottom = min(self.height, int(self._mouth_cy + half))
            for y in range(top, bottom):
                start = (y * w + left) * 4
                frame[start : start + len(row_fill)] = row_fill

        return bytes(frame)


class MuseTalkEngine:
    """The real engine. Not implemented yet.

    When it lands, construction loads the prepared avatar bundle (coords,
    latents, masks) plus whisper-tiny, the UNet and the VAE decoder, and
    render() runs whisper -> UNet -> VAE decode -> blend for one audio window.
    Weights belong in the asset directory the plugin logs at startup, under
    background-models/musetalk/.
    """

    name = "musetalk"
    real_model = True

    # MuseTalk's own inpainting resolution; the blend back into the full frame
    # happens worker-side, so this is what the plugin will receive.
    width = 256
    height = 256

    @staticmethod
    def available(asset_path: str) -> tuple[bool, str]:
        if not asset_path:
            return False, "no avatar bundle was given"
        if not os.path.isdir(asset_path):
            return False, f"avatar bundle '{asset_path}' is not a prepared directory"
        return False, "the MuseTalk engine is not implemented yet (stage C)"


# --------------------------------------------------------------------------
# Session
# --------------------------------------------------------------------------


class Session:
    def __init__(self, stream, fps: int, sample_rate: int, engine) -> None:
        self.stream = stream
        self.fps = fps
        self.sample_rate = sample_rate
        self.engine = engine

        # One frame per 1/fps of audio, which is also how the real engine will
        # be driven: a frame belongs to an audio window, not to a wall clock.
        self.samples_per_frame = max(1, sample_rate // fps)

        self.buffer: list[float] = []
        self.buffer_pts: int | None = None
        self.frames = 0
        self.dropped = 0
        self.render_us = 0
        self.last_stat = time.monotonic()

    def on_audio(self, payload: bytes) -> None:
        if len(payload) < AUDIO_HEADER.size:
            raise ProtocolError("AUDIO payload truncated")

        pts_ns, count = AUDIO_HEADER.unpack_from(payload, 0)
        expected = AUDIO_HEADER.size + count * 4
        if len(payload) != expected:
            raise ProtocolError(f"AUDIO says {count} samples but carries {len(payload)} bytes")

        samples = struct.unpack_from(f"<{count}f", payload, AUDIO_HEADER.size) if count else ()

        if self.buffer_pts is None:
            self.buffer_pts = pts_ns

        self.buffer.extend(samples)

        while len(self.buffer) >= self.samples_per_frame:
            window = self.buffer[: self.samples_per_frame]
            del self.buffer[: self.samples_per_frame]

            window_pts = self.buffer_pts
            self.buffer_pts += int(self.samples_per_frame * 1_000_000_000 / self.sample_rate)

            self.emit_frame(window, window_pts)

    def emit_frame(self, window, pts_ns: int) -> None:
        peak = max((abs(s) for s in window), default=0.0)
        rms = math.sqrt(sum(s * s for s in window) / len(window)) if window else 0.0

        started = time.perf_counter()
        pixels = self.engine.render(rms, peak)
        self.render_us = int((time.perf_counter() - started) * 1_000_000)

        header = FRAME_HEADER.pack(pts_ns, self.engine.width, self.engine.height)
        send_message(self.stream, MSG_FRAME, header + pixels)
        self.frames += 1

        now = time.monotonic()
        if now - self.last_stat >= 1.0:
            self.last_stat = now
            # The stub has no model stages, so those stay zero rather than
            # being filled with its own render time - a number in the unet
            # field that did not come from a UNet would be a lie the moment
            # anyone reads these stats to make a performance decision. The
            # stub's cost goes in a log line instead, where it cannot be
            # mistaken for the model's. queue_depth is samples still short of
            # a full frame, not a frame backlog.
            send_message(
                self.stream,
                MSG_STAT,
                STAT_PAYLOAD.pack(0, 0, 0, len(self.buffer), self.dropped),
            )
            send_log(
                self.stream,
                LOG_DEBUG,
                f"stub render {self.render_us} us/frame, {self.frames} frames sent",
            )

    def run(self) -> None:
        while True:
            msg_type, payload = read_message(self.stream)

            if msg_type == MSG_AUDIO:
                self.on_audio(payload)
            elif msg_type == MSG_BYE:
                return
            else:
                # Forward compatibility in the same spirit as the plugin side:
                # consume and ignore, do not drop a working pipe.
                send_log(self.stream, LOG_DEBUG, f"ignoring message type {msg_type}")


def handshake(stream, requested_engine: str):
    msg_type, payload = read_message(stream)
    if msg_type != MSG_HELLO:
        raise ProtocolError(f"expected HELLO, got message type {msg_type}")
    if len(payload) < HELLO_PAYLOAD.size:
        raise ProtocolError("HELLO payload truncated")

    version, sample_rate, fps, asset_len = HELLO_PAYLOAD.unpack_from(payload, 0)
    asset_path = payload[HELLO_PAYLOAD.size : HELLO_PAYLOAD.size + asset_len].decode("utf-8", "replace")

    if version != PROTOCOL_VERSION:
        send_error(stream, 1, f"plugin speaks protocol {version}, this worker speaks {PROTOCOL_VERSION}")
        raise ProtocolError(f"protocol mismatch: plugin {version}, worker {PROTOCOL_VERSION}")

    if fps <= 0 or sample_rate <= 0:
        send_error(stream, 2, f"unusable parameters: {sample_rate} Hz, {fps} fps")
        raise ProtocolError("unusable HELLO parameters")

    engine = None
    if requested_engine in ("auto", "musetalk"):
        ok, why = MuseTalkEngine.available(asset_path)
        if ok:
            engine = MuseTalkEngine()
        elif requested_engine == "musetalk":
            send_error(stream, 3, f"MuseTalk unavailable: {why}")
            raise ProtocolError(f"MuseTalk unavailable: {why}")
        else:
            engine = StubEngine()
            send_log(stream, LOG_WARNING, f"falling back to the stub engine: {why}")
    else:
        engine = StubEngine()

    flags = READY_REAL_MODEL if engine.real_model else 0
    name = engine.name.encode("utf-8")
    send_message(
        stream,
        MSG_READY,
        struct.pack("<IIIII", PROTOCOL_VERSION, engine.width, engine.height, flags, len(name)) + name,
    )

    print(f"ready: engine={engine.name} {engine.width}x{engine.height} "
          f"{sample_rate} Hz {fps} fps asset={asset_path or '(none)'}", flush=True)

    return Session(stream, fps, sample_rate, engine)


def connect(pipe_name: str, timeout_s: float):
    """The plugin creates the pipe when the source is enabled, so a worker
    started first has to wait for it rather than fail."""
    deadline = time.monotonic() + timeout_s
    while True:
        try:
            return open(pipe_name, "r+b", buffering=0)
        except FileNotFoundError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.25)


# --------------------------------------------------------------------------
# Self-test: the same byte fixtures avatar-protocol-test.c pins
# --------------------------------------------------------------------------


def selftest() -> int:
    # FRAME (type 4) with a 16-byte payload.
    expected_header = bytes.fromhex("10000000" "0400" "0000")
    assert HEADER.pack(16, MSG_FRAME, 0) == expected_header, HEADER.pack(16, MSG_FRAME, 0).hex()
    assert HEADER.unpack(expected_header) == (16, MSG_FRAME, 0)

    # pts_ns = 1000000000, two samples: 0.5 and -0.25.
    expected_audio = bytes.fromhex(
        "00CA9A3B00000000"  # pts_ns
        "02000000"  # sample_count
        "0000003F"  # 0.5f
        "000080BE"  # -0.25f
    )
    built = AUDIO_HEADER.pack(1000000000, 2) + struct.pack("<2f", 0.5, -0.25)
    assert built == expected_audio, built.hex()

    pts, count = AUDIO_HEADER.unpack_from(expected_audio, 0)
    assert pts == 1000000000 and count == 2
    assert struct.unpack_from("<2f", expected_audio, AUDIO_HEADER.size) == (0.5, -0.25)

    # Sizes the C side asserts as constants.
    assert HEADER.size == HEADER_SIZE
    assert AUDIO_HEADER.size == 12
    assert FRAME_HEADER.size == 16
    assert STAT_PAYLOAD.size == 20

    # Signed timestamps have to survive the extremes.
    for value in (-(2**63), 2**63 - 1, -1, 0):
        assert struct.unpack("<q", struct.pack("<q", value))[0] == value

    # The stub's frame has to match what it announces, or the plugin will
    # reject it by the same validation the C test covers.
    engine = StubEngine()
    pixels = engine.render(0.1, 0.5)
    assert len(pixels) == engine.width * engine.height * 4, len(pixels)
    assert engine.width <= FRAME_MAX_EDGE and engine.height <= FRAME_MAX_EDGE
    assert FRAME_HEADER.size + len(pixels) <= MAX_PAYLOAD

    # A shut mouth and an open one must actually differ, otherwise the stub
    # proves nothing when watched on screen.
    assert engine.render(0.0, 0.0) != engine.render(0.5, 0.9)

    print("selftest: ok")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="ppobs avatar lip-sync worker")
    parser.add_argument("--pipe", help=r"named pipe created by the plugin, e.g. \\.\pipe\ppobs-avatar-123-0")
    parser.add_argument("--engine", default="auto", choices=["auto", "musetalk", "stub"])
    parser.add_argument("--connect-timeout", type=float, default=30.0)
    parser.add_argument("--selftest", action="store_true", help="check the wire format and exit")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    if not args.pipe:
        parser.error("--pipe is required unless --selftest is given")

    print(f"connecting to {args.pipe}", flush=True)
    stream = connect(args.pipe, args.connect_timeout)

    try:
        session = handshake(stream, args.engine)
        session.run()
        print("plugin said goodbye", flush=True)
    except ProtocolError as exc:
        print(f"protocol error: {exc}", file=sys.stderr, flush=True)
        return 1
    finally:
        stream.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
