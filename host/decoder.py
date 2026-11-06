"""Decode the HartWatch UART1 stream. Pure functions; the format is in docs/DESIGN.md."""
from __future__ import annotations

import zlib
from dataclasses import dataclass, field
from typing import Iterator

MASK64 = (1 << 64) - 1
SEQ_MASK = (1 << 32) - 1
TYPE_SAMPLES = 1
TYPE_END = 2
FIELDS = ("seq", "mtime", "mcycle", "minstret", "hpm0", "hpm1", "hpm2", "mepc")
VARINT_MAX = 10


class DecodeError(ValueError):
    """A frame is damaged. The stream can continue from the next 0x00."""


@dataclass(frozen=True)
class Sample:
    hart: int
    seq: int
    mtime: int
    mcycle: int
    minstret: int
    hpm0: int
    hpm1: int
    hpm2: int
    mepc: int


@dataclass(frozen=True)
class SampleFrame:
    hart: int
    frame_seq: int
    dropped: int
    samples: tuple[Sample, ...]


@dataclass(frozen=True)
class EndFrame:
    hart: int
    frame_seq: int
    dropped: int
    attempts: int
    drained: int
    bad: int
    has_digest: bool
    digest: int


Frame = SampleFrame | EndFrame


def unzigzag(value: int) -> int:
    return (value >> 1) ^ -(value & 1)


def read_varint(data: bytes, pos: int) -> tuple[int, int]:
    """Return (value, next position)."""
    value = 0
    for i in range(VARINT_MAX):
        if pos >= len(data):
            raise DecodeError("varint runs past the end of the frame")
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << (7 * i)
        if byte < 0x80:
            if value > MASK64:
                raise DecodeError("varint does not fit in 64 bits")
            return value, pos
    raise DecodeError("varint is too long")


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    pos = 0
    while pos < len(data):
        code = data[pos]
        if code == 0:
            raise DecodeError("0x00 inside a COBS block")
        pos += 1
        end = pos + code - 1
        if end > len(data):
            raise DecodeError("COBS block runs past the end of the frame")
        out += data[pos:end]
        pos = end
        if code < 0xFF and pos < len(data):
            out.append(0)
    return bytes(out)


def check_crc(payload: bytes) -> bytes:
    """Return the payload without its CRC, or raise if it does not match."""
    if len(payload) < 4:
        raise DecodeError("frame too short for a CRC")
    body, tail = payload[:-4], payload[-4:]
    if zlib.crc32(body) != int.from_bytes(tail, "little"):
        raise DecodeError("CRC mismatch")
    return body


def parse_samples(body: bytes, pos: int, hart: int) -> tuple[Sample, ...]:
    samples: list[Sample] = []
    prev = [0] * len(FIELDS)
    while pos < len(body):
        cur: list[int] = []
        for i in range(len(FIELDS)):
            delta, pos = read_varint(body, pos)
            cur.append((prev[i] + unzigzag(delta)) & MASK64)
        prev = cur
        samples.append(Sample(hart, *cur))
    return tuple(samples)


def parse_payload(body: bytes) -> Frame:
    if len(body) < 4:
        raise DecodeError("frame too short for a header")
    frame_type, hart = body[0], body[1]
    frame_seq, pos = read_varint(body, 2)
    dropped, pos = read_varint(body, pos)
    if frame_type == TYPE_SAMPLES:
        return SampleFrame(hart, frame_seq, dropped, parse_samples(body, pos, hart))
    if frame_type == TYPE_END:
        attempts, pos = read_varint(body, pos)
        drained, pos = read_varint(body, pos)
        bad, pos = read_varint(body, pos)
        if pos >= len(body):
            raise DecodeError("END frame too short")
        has_digest = body[pos] != 0
        digest, pos = read_varint(body, pos + 1)
        if pos != len(body):
            raise DecodeError("trailing bytes in END frame")
        return EndFrame(hart, frame_seq, dropped, attempts, drained, bad, has_digest, digest)
    raise DecodeError(f"unknown frame type {frame_type}")


def decode_frame(raw: bytes) -> Frame:
    """Decode one frame, given its bytes without the 0x00 delimiter."""
    return parse_payload(check_crc(cobs_decode(raw)))


def split_frames(stream: bytes) -> Iterator[bytes]:
    """Yield the bytes between 0x00 delimiters. An unfinished last frame is not yielded."""
    start = 0
    while True:
        end = stream.find(b"\x00", start)
        if end < 0:
            return
        if end > start:
            yield stream[start:end]
        start = end + 1


def seq_gap(prev: int, cur: int) -> int | None:
    """Samples skipped between two seq values, or None if cur is not newer. seq is 32 bits and
    wraps, so the difference is taken modulo 2^32."""
    step = (cur - prev) & SEQ_MASK
    if step == 0 or step >= 1 << 31:
        return None
    return step - 1


def _rotl7(value: int) -> int:
    return ((value << 7) | (value >> 57)) & MASK64


def sample_digest(sample: Sample) -> int:
    """Must match sample_digest() in fw/src/sample.c. flags is always 0 on the wire."""
    acc = sample.seq | (sample.hart << 32)
    for name in FIELDS[1:]:
        acc = _rotl7(acc) ^ getattr(sample, name)
    return acc


def fold_digest(running: int, sample: Sample) -> int:
    """One step of the per-hart running digest that END frames carry."""
    return _rotl7(running) ^ sample_digest(sample)


@dataclass
class HartStream:
    """What the stream says about one hart."""

    frames: int = 0
    wire_bytes: int = 0
    samples: list[Sample] = field(default_factory=list)
    frame_seqs: list[int] = field(default_factory=list)
    dropped: int = 0
    end: EndFrame | None = None


@dataclass
class Stream:
    harts: dict[int, HartStream] = field(default_factory=dict)
    bad_frames: int = 0
    total_bytes: int = 0


def decode_stream(data: bytes) -> Stream:
    """Decode a whole capture. Damaged frames are counted and skipped."""
    stream = Stream(total_bytes=len(data))
    for raw in split_frames(data):
        try:
            frame = decode_frame(raw)
        except DecodeError:
            stream.bad_frames += 1
            continue
        hart = stream.harts.setdefault(frame.hart, HartStream())
        hart.frames += 1
        hart.wire_bytes += len(raw) + 1
        hart.frame_seqs.append(frame.frame_seq)
        hart.dropped = max(hart.dropped, frame.dropped)
        if isinstance(frame, SampleFrame):
            hart.samples.extend(frame.samples)
        else:
            hart.end = frame
    return stream
