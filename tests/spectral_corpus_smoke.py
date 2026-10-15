"""End-to-end WAV decoder and JSONL smoke test for the research tool."""

import json
import math
import os
import struct
import subprocess
import sys
import tempfile


def wav_bytes(format_code, bits, channels=1, frames=48):
    samples = []
    for index in range(frames):
        value = 0.5 * math.sin(2.0 * math.pi * index / 12.0)
        for channel in range(channels):
            channel_value = value if channel == 0 else -value
            if format_code == 3:
                samples.append(struct.pack("<f", channel_value))
            elif bits == 8:
                samples.append(bytes((int(round(channel_value * 127.0 + 128.0)),)))
            elif bits == 16:
                samples.append(struct.pack("<h", int(channel_value * 32767.0)))
            elif bits == 24:
                raw = int(channel_value * 8388607.0)
                samples.append(int(raw).to_bytes(3, "little", signed=True))
            elif bits == 32:
                samples.append(struct.pack("<i", int(channel_value * 2147483647.0)))
    payload = b"".join(samples)
    bytes_per_sample = 4 if format_code == 3 else bits // 8
    block_align = channels * bytes_per_sample
    fmt = struct.pack("<HHIIHH", format_code, channels, 48000,
                      48000 * block_align, block_align, bits)
    body = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    body += b"data" + struct.pack("<I", len(payload)) + payload
    return b"RIFF" + struct.pack("<I", len(body) + 4) + b"WAVE" + body


def run(executable, path, mode="amplitude"):
    result = subprocess.run([executable, "--input", path, "--mode", mode],
                            text=True, capture_output=True, check=False)
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    records = [json.loads(line) for line in result.stdout.splitlines() if line]
    assert records and records[0]["kind"] == "metadata"
    assert records[0]["input"] == path
    assert any(record["kind"] == "audio_span" for record in records)


def expect_failure(executable, path):
    result = subprocess.run([executable, "--input", path],
                            text=True, capture_output=True, check=False)
    assert result.returncode != 0


def main():
    executable = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="speech animation тест ") as directory:
        cases = [(1, 8, 1), (1, 16, 1), (1, 24, 1),
                 (1, 32, 1), (3, 32, 1), (1, 16, 2)]
        for index, (format_code, bits, channels) in enumerate(cases):
            path = os.path.join(directory, "voice sample %d.wav" % index)
            with open(path, "wb") as output:
                output.write(wav_bytes(format_code, bits, channels))
            run(executable, path, "amplitude")
            run(executable, path, "spectral")

        malformed = os.path.join(directory, "bad.wav")
        payload = bytearray(wav_bytes(1, 16))
        payload[4:8] = struct.pack("<I", 1)
        with open(malformed, "wb") as output:
            output.write(payload)
        expect_failure(executable, malformed)

        unsupported = os.path.join(directory, "unsupported.wav")
        payload = wav_bytes(1, 16)
        payload = payload[:20] + struct.pack("<H", 6) + payload[22:]
        with open(unsupported, "wb") as output:
            output.write(payload)
        expect_failure(executable, unsupported)

        unaligned = os.path.join(directory, "unaligned.wav")
        payload = bytearray(wav_bytes(1, 16))
        payload.extend(b"x")
        payload[40:44] = struct.pack("<I", 97)
        payload[4:8] = struct.pack("<I", len(payload) - 8)
        with open(unaligned, "wb") as output:
            output.write(payload)
        expect_failure(executable, unaligned)


if __name__ == "__main__":
    main()
