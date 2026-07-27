#
# MicroPython Codec2 encode/decode utility
# Copyright (c) 2026 8bitmcu
# License: MIT
#
# Codec2 itself has no self-describing file format -- its own c2enc/c2dec
# tools just emit a headerless stream of packed frames, and you're expected
# to already know which mode was used to decode it. We define a minimal
# container instead (magic + mode byte) so a .c2 file carries what it needs
# to decode itself.
#

import os
import struct
import codec2

SAMPLE_RATE = 8000  # codec2 is hardcoded to 8kHz internally, all modes
CHANNELS = 1
BITS_PER_SAMPLE = 16

C2_MAGIC = b"C2F1"
C2_HEADER_LEN = 5  # magic (4) + mode (1)

DEFAULT_ENCODE_MODE = codec2.MODE_1200


def _wav_read_header(f):
    """Reads/validates a WAV header from an open file, leaving the file
    position at the start of the data chunk. Returns the data chunk size
    in bytes. Raises ValueError if the file isn't mono 16-bit PCM @ 8kHz."""
    riff = f.read(12)
    if len(riff) < 12 or riff[0:4] != b"RIFF" or riff[8:12] != b"WAVE":
        raise ValueError("not a WAV file")

    channels = None
    sample_rate = None
    bits_per_sample = None

    while True:
        chunk_hdr = f.read(8)
        if len(chunk_hdr) < 8:
            raise ValueError("WAV file has no data chunk")
        chunk_id = chunk_hdr[0:4]
        chunk_size = struct.unpack("<I", chunk_hdr[4:8])[0]
        pad = chunk_size & 1

        if chunk_id == b"fmt ":
            fmt = f.read(chunk_size)
            audio_format, channels, sample_rate, _, _, bits_per_sample = \
                struct.unpack("<HHIIHH", fmt[0:16])
            if audio_format != 1:
                raise ValueError("only uncompressed PCM WAV is supported")
            f.seek(pad, 1)
        elif chunk_id == b"data":
            if channels is None:
                raise ValueError("WAV data chunk appears before fmt chunk")
            if (channels, sample_rate, bits_per_sample) != \
               (CHANNELS, SAMPLE_RATE, BITS_PER_SAMPLE):
                raise ValueError(
                    f"input must be {SAMPLE_RATE}Hz mono {BITS_PER_SAMPLE}-bit "
                    f"PCM (got {sample_rate}Hz, {channels}ch, {bits_per_sample}-bit)")
            return chunk_size
        else:
            f.seek(chunk_size + pad, 1)

def _wav_write_header(f, data_bytes):
    byte_rate = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE // 8)
    block_align = CHANNELS * (BITS_PER_SAMPLE // 8)
    f.write(b"RIFF")
    f.write(struct.pack("<I", 36 + data_bytes))
    f.write(b"WAVE")
    f.write(b"fmt ")
    f.write(struct.pack("<IHHIIHH", 16, 1, CHANNELS, SAMPLE_RATE,
                        byte_rate, block_align, BITS_PER_SAMPLE))
    f.write(b"data")
    f.write(struct.pack("<I", data_bytes))

def encode(input_path, output_path, mode):
    """input.wav (8kHz mono 16-bit PCM) -> output (.c2 container)."""
    c2 = codec2.Codec2(mode)
    try:
        frame_pcm_bytes = c2.samples_per_frame() * 2
        frames = 0
        with open(input_path, "rb") as fin, open(output_path, "wb") as fout:
            data_bytes = _wav_read_header(fin)
            fout.write(C2_MAGIC)
            fout.write(struct.pack("<B", mode))

            remaining = data_bytes
            while remaining > 0:
                pcm = fin.read(min(frame_pcm_bytes, remaining))
                remaining -= len(pcm)
                if len(pcm) < frame_pcm_bytes:
                    if len(pcm) == 0:
                        break
                    pcm = pcm + b"\x00" * (frame_pcm_bytes - len(pcm))
                fout.write(c2.encode(pcm))
                frames += 1
        return frames
    finally:
        c2.deinit()

def decode(input_path, output_path):
    """input (.c2 container) -> output.wav (8kHz mono 16-bit PCM)."""
    with open(input_path, "rb") as fin:
        header = fin.read(C2_HEADER_LEN)
        if len(header) < C2_HEADER_LEN or header[0:4] != C2_MAGIC:
            raise ValueError(f"{input_path} is not a .c2 file")
        mode = header[4]

    c2 = codec2.Codec2(mode)
    try:
        bytes_per_frame = c2.bytes_per_frame()
        frame_pcm_bytes = c2.samples_per_frame() * 2

        total_size = os.stat(input_path)[6]
        frame_count = (total_size - C2_HEADER_LEN) // bytes_per_frame
        data_bytes = frame_count * frame_pcm_bytes

        with open(input_path, "rb") as fin, open(output_path, "wb") as fout:
            fin.seek(C2_HEADER_LEN)
            _wav_write_header(fout, data_bytes)
            for _ in range(frame_count):
                frame = fin.read(bytes_per_frame)
                if len(frame) < bytes_per_frame:
                    break
                fout.write(c2.decode(frame))
        return frame_count
    finally:
        c2.deinit()

def main(env, args):
    if len(args) != 3 or args[0] not in ("encode", "decode"):
        print("Usage: c2 <encode|decode> <input.wav> <output.wav>")
        return

    action, input_path, output_path = args

    if action == "encode":
        frames = encode(input_path, output_path, DEFAULT_ENCODE_MODE)
        print(f"Encoded {frames} frame(s) to: {output_path}")
    else:
        frames = decode(input_path, output_path)
        print(f"Decoded {frames} frame(s) to: {output_path}")
