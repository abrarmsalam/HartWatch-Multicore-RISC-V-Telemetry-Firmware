"""Unit tests for the decoder. The vectors come from the real C encoder (make host-test)."""
from __future__ import annotations

import os
import zlib
from pathlib import Path

import pytest

import decoder as d

BUILD = Path(os.environ.get("HW_BUILD", "build"))


def frame_bytes(payload: bytes) -> bytes:
    """A reference encoder, written separately from the C one."""
    body = payload + zlib.crc32(payload).to_bytes(4, "little")
    out = bytearray()
    for block in body.split(b"\x00"):
        while len(block) >= 254:
            out += b"\xff" + block[:254]
            block = block[254:]
        out += bytes([len(block) + 1]) + block
    return bytes(out) + b"\x00"


def varint(value: int) -> bytes:
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def zigzag(value: int) -> int:
    return ((value << 1) ^ (value >> 63)) & d.MASK64


def samples_payload(hart: int = 1, frame_seq: int = 0, dropped: int = 0,
                    rows: tuple[tuple[int, ...], ...] = ((1, 2, 3, 4, 5, 6, 7, 8),)) -> bytes:
    out = bytes([d.TYPE_SAMPLES, hart]) + varint(frame_seq) + varint(dropped)
    prev = (0,) * 8
    for row in rows:
        for a, b in zip(row, prev):
            out += varint(zigzag(a - b))
        prev = row
    return out


@pytest.mark.parametrize("encoded,value", [(0, 0), (1, -1), (2, 1), (3, -2), (4294967294, 2147483647),
                                           (d.MASK64, -(1 << 63))])
def test_unzigzag(encoded: int, value: int) -> None:
    assert d.unzigzag(encoded) == value


@pytest.mark.parametrize("data,value,end", [(b"\x00", 0, 1), (b"\x7f", 127, 1), (b"\x80\x01", 128, 2),
                                            (b"\xac\x02", 300, 2), (b"\xff" * 9 + b"\x01", d.MASK64, 10)])
def test_read_varint(data: bytes, value: int, end: int) -> None:
    assert d.read_varint(data, 0) == (value, end)


@pytest.mark.parametrize("data", [b"", b"\x80", b"\xff" * 11, b"\xff" * 9 + b"\x7f"])
def test_read_varint_rejects_bad_input(data: bytes) -> None:
    with pytest.raises(d.DecodeError):
        d.read_varint(data, 0)


@pytest.mark.parametrize("encoded,plain", [
    (b"\x01\x01", b"\x00"),
    (b"\x01\x01\x01", b"\x00\x00"),
    (b"\x03\x11\x22\x02\x33", b"\x11\x22\x00\x33"),
    (b"\x05\x11\x22\x33\x44", b"\x11\x22\x33\x44"),
    (b"\x02\x11\x01\x01\x01", b"\x11\x00\x00\x00"),
    (b"\xff" + b"A" * 254, b"A" * 254),
    (b"\xff" + b"A" * 254 + b"\x02A", b"A" * 255),
])
def test_cobs_decode(encoded: bytes, plain: bytes) -> None:
    assert d.cobs_decode(encoded) == plain


@pytest.mark.parametrize("data", [b"\x05\x11", b"\x02\x11\x00"])
def test_cobs_decode_rejects_bad_input(data: bytes) -> None:
    with pytest.raises(d.DecodeError):
        d.cobs_decode(data)


def test_decode_reference_frame() -> None:
    frame = d.decode_frame(frame_bytes(samples_payload(hart=3, frame_seq=9, dropped=4))[:-1])
    assert isinstance(frame, d.SampleFrame)
    assert (frame.hart, frame.frame_seq, frame.dropped) == (3, 9, 4)
    assert frame.samples == (d.Sample(3, 1, 2, 3, 4, 5, 6, 7, 8),)


def test_deltas_accumulate_and_may_be_negative() -> None:
    rows = ((10, 20, 30, 40, 50, 60, 70, 0x80001000), (11, 25, 31, 41, 50, 60, 70, 0x80000F00))
    frame = d.decode_frame(frame_bytes(samples_payload(rows=rows))[:-1])
    assert frame.samples[1] == d.Sample(1, *rows[1])


def test_any_single_bit_flip_is_rejected_or_harmless_to_data() -> None:
    raw = frame_bytes(samples_payload(rows=((1, 2, 3, 4, 5, 6, 7, 8), (2, 4, 6, 8, 9, 9, 9, 0x80000000))))[:-1]
    good = d.decode_frame(raw)
    for bit in range(len(raw) * 8):
        damaged = bytearray(raw)
        damaged[bit // 8] ^= 1 << (bit % 8)
        if 0 in damaged:
            continue    # a flip that makes 0x00 splits the frame; decode_stream covers that
        try:
            assert d.decode_frame(bytes(damaged)) == good
        except d.DecodeError:
            pass


def test_truncated_body_in_a_valid_crc_is_rejected() -> None:
    with pytest.raises(d.DecodeError):
        d.decode_frame(frame_bytes(samples_payload()[:-1] + b"\x80")[:-1])


def test_unknown_type_is_rejected() -> None:
    with pytest.raises(d.DecodeError):
        d.decode_frame(frame_bytes(bytes([9, 1, 0, 0]))[:-1])


def test_stream_resynchronises_after_damage() -> None:
    a = frame_bytes(samples_payload(frame_seq=0))
    b = frame_bytes(samples_payload(frame_seq=1))
    c = frame_bytes(samples_payload(frame_seq=2))
    damaged = bytearray(b)
    damaged[5] ^= 0x40
    stream = d.decode_stream(a + bytes(damaged) + c)
    assert stream.bad_frames == 1
    assert stream.harts[1].frame_seqs == [0, 2]


def test_stream_starting_mid_frame_recovers() -> None:
    a = frame_bytes(samples_payload(frame_seq=0))
    b = frame_bytes(samples_payload(frame_seq=1))
    stream = d.decode_stream(a[3:] + b)
    assert stream.harts[1].frame_seqs == [1]


def test_unfinished_last_frame_is_ignored() -> None:
    a = frame_bytes(samples_payload(frame_seq=0))
    assert d.decode_stream(a + a[:-3]).bad_frames == 0


def test_digest_matches_c_golden_value() -> None:
    """0x223c is the checksum test_sample.c checks, for this record with flags 0x5A."""
    sample = d.Sample(2, 0x01020304 | (0x5A << 40), 0x1111111111111111, 0x2222222222222222,
                      0x3333333333333333, 0x4444444444444444, 0x5555555555555555,
                      0x6666666666666666, 0x80001234)
    acc = d.sample_digest(sample)
    assert (acc ^ (acc >> 16) ^ (acc >> 32) ^ (acc >> 48)) & 0xFFFF == 0x223C


def test_c_encoder_round_trips_through_python_decoder() -> None:
    stream = d.decode_stream((BUILD / "wire_vectors.bin").read_bytes())
    expected = [tuple(int(x) for x in line.split())
                for line in (BUILD / "wire_vectors.txt").read_text().splitlines()]
    assert stream.bad_frames == 0
    assert len(expected) > 1000
    by_hart: dict[int, list[tuple[int, ...]]] = {}
    for row in expected:
        by_hart.setdefault(row[0], []).append(row)
    for hart, rows in by_hart.items():
        got = [(s.hart, s.seq, s.mtime, s.mcycle, s.minstret, s.hpm0, s.hpm1, s.hpm2, s.mepc)
               for s in stream.harts[hart].samples]
        assert got == rows
