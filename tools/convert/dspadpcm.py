"""DSP-ADPCM, the 3DS sound chip's own ADPCM (the GameCube's): the DSP decodes it, so a sound goes from the card to
the channel without the CPU decoding it, at a little over a quarter of PCM16's memory.

  import dspadpcm
  coefs, frames = dspadpcm.encode(samples)          # samples: int16 numpy array
  data = dspadpcm.snd3(samples, rate)               # a whole 'SND3' file (source/mwsound/soundmanagerimp.cpp)
  python dspadpcm.py <in.wav> [--out decoded.wav]   # encodes, decodes again and prints the signal to noise ratio

A frame is 8 bytes for 14 samples. Byte 0: the predictor (high nibble, 0..7) and the scale's exponent (low nibble);
bytes 1..7: 14 signed 4-bit residuals, the high nibble first. A sample is
  clamp16((((r << exp) << 11) + 1024 + c1 * h1 + c2 * h2) >> 11)
with (c1, c2) the predictor's pair of the file's 8 (11 fractional bits) and h1, h2 the last two samples decoded.

'SND3' file: 'SND3', u32 rate, u32 samples, s16 coefs[16], u32 envelope bytes, the frames (ceil(samples / 14) x 8
bytes), then the envelope: a byte per 256 samples, sqrt(RMS) x 255 (the talking mouth reads it; music has none).

The encoder: the 8 predictors come from the sound itself (frames assigned to the predictor that leaves the least
residual, each predictor the least-squares fit of its frames, a few rounds); each frame then tries every predictor at
two scales and keeps the one decoding closest. The quantizing follows the decoder sample by sample; to run in numpy
the sound is cut into pieces of 64 frames encoded side by side, each starting from the source's own last two samples
(the decoder's differ from them by one quantizing step, which dies away within a few samples).
"""
import struct
import sys

import numpy as np

FRAME = 14              # samples a frame
SEGMENT = 64            # frames a piece encoded side by side
ENV_BLOCK = 256         # samples an envelope byte


def _predictors(x):
    """The 8 predictor pairs (floats) for the source x (float64, a whole number of frames)."""
    f = x.reshape(-1, FRAME)
    x1 = np.concatenate(([0.0], x[:-1])).reshape(-1, FRAME)
    x2 = np.concatenate(([0.0, 0.0], x[:-2])).reshape(-1, FRAME)
    r01, r02 = (f * x1).sum(1), (f * x2).sum(1)
    r11, r12, r22 = (x1 * x1).sum(1), (x1 * x2).sum(1), (x2 * x2).sum(1)
    # a start covering speech and music: silence, low-pass of each strength, a resonance or two
    a = np.array([[0.0, 0.0], [0.9, 0.0], [1.95, -0.96], [1.8, -0.83], [1.5, -0.6], [1.2, -0.35], [0.4, 0.2],
                  [-0.5, 0.0]])
    for _ in range(8):
        # residual energy of every frame under every predictor (the frame's own energy left out: the same for all)
        e = (-2 * a[:, 0, None] * r01 - 2 * a[:, 1, None] * r02 + a[:, 0, None] ** 2 * r11
             + 2 * a[:, 0, None] * a[:, 1, None] * r12 + a[:, 1, None] ** 2 * r22)
        pick = e.argmin(0)
        for p in range(8):
            m = pick == p
            if not m.any():
                continue
            s11, s12, s22 = r11[m].sum(), r12[m].sum(), r22[m].sum()
            s01, s02 = r01[m].sum(), r02[m].sum()
            ridge = 1e-6 * (s11 + s22) + 1.0
            det = (s11 + ridge) * (s22 + ridge) - s12 * s12
            if det <= 0:
                continue
            a[p] = [(s01 * (s22 + ridge) - s02 * s12) / det, (s02 * (s11 + ridge) - s01 * s12) / det]
    # kept stable (poles inside the unit circle) with a margin: |a2| < 1, |a1| < 1 - a2
    a[:, 1] = np.clip(a[:, 1], -0.97, 0.97)
    lim = (1.0 - a[:, 1]) * 0.995
    a[:, 0] = np.clip(a[:, 0], -lim, lim)
    return a


def encode(samples):
    """(coefs: int16 array of 16, frames: bytes) for int16 samples."""
    n = len(samples)
    nf = max(1, -(-n // FRAME))
    x = np.zeros(nf * FRAME, dtype=np.float64)
    x[:n] = samples
    coefs = np.round(_predictors(x) * 2048).astype(np.int64)
    c1, c2 = coefs[:, 0], coefs[:, 1]

    # pieces of SEGMENT frames side by side: k pieces, frame j of each at once
    k = -(-nf // SEGMENT)
    xp = np.zeros(k * SEGMENT * FRAME, dtype=np.int64)
    xp[:n] = samples
    seg = xp.reshape(k, SEGMENT, FRAME)
    starts = np.arange(k) * SEGMENT * FRAME
    h1 = np.where(starts >= 1, xp[np.maximum(starts - 1, 0)], 0)
    h2 = np.where(starts >= 2, xp[np.maximum(starts - 2, 0)], 0)

    # the scale each predictor needs, from the source's own history: tried as is and one step up
    xs = xp.astype(np.float64)
    p1 = np.concatenate(([0.0], xs[:-1]))
    p2 = np.concatenate(([0.0, 0.0], xs[:-2]))
    res = xs[None, :] - (c1[:, None] * p1[None, :] + c2[:, None] * p2[None, :]) / 2048.0
    peak = np.abs(res).reshape(8, k, SEGMENT, FRAME).max(3)                      # (8, k, SEGMENT)
    exp0 = np.clip(np.ceil(np.log2(np.maximum(peak, 1e-9) / 7.0)), 0, 12).astype(np.int64)
    cand_p = np.repeat(np.arange(8), 2)                                          # 16 candidates
    cand_up = np.tile([0, 1], 8)

    heads = np.zeros((k, SEGMENT), dtype=np.uint8)
    nibs = np.zeros((k, SEGMENT, FRAME), dtype=np.int64)
    rows = np.arange(k)
    cc1, cc2 = c1[cand_p][None, :], c2[cand_p][None, :]
    for j in range(SEGMENT):
        ex = np.minimum(exp0[cand_p, :, j].T + cand_up[None, :], 12)              # (k, 16)
        step = np.left_shift(1, ex)
        a1 = np.repeat(h1[:, None], 16, 1)
        a2 = np.repeat(h2[:, None], 16, 1)
        err = np.zeros((k, 16))
        r_all = np.zeros((k, 16, FRAME), dtype=np.int64)
        for i in range(FRAME):
            xi = seg[:, j, i][:, None]
            pred = cc1 * a1 + cc2 * a2
            r = np.clip(np.rint((xi * 2048 - pred) / (step * 2048.0)), -8, 7).astype(np.int64)
            out = np.clip((r * step * 2048 + 1024 + pred) >> 11, -32768, 32767)
            d = xi - out
            err += d * d
            r_all[:, :, i] = r
            a2, a1 = a1, out
        best = err.argmin(1)
        heads[:, j] = (cand_p[best] << 4) | ex[rows, best]
        nibs[:, j] = r_all[rows, best]
        h1, h2 = a1[rows, best], a2[rows, best]

    heads = heads.reshape(-1)[:nf]
    nibs = (nibs.reshape(-1, FRAME)[:nf] & 15).astype(np.uint8)
    out = np.zeros((nf, 8), dtype=np.uint8)
    out[:, 0] = heads
    out[:, 1:] = (nibs[:, 0::2] << 4) | nibs[:, 1::2]
    return coefs.reshape(-1).astype(np.int16), out.tobytes()


def decode(coefs, frames, n):
    """The DSP's decoding, for checking the encoder (slow: a sample at a time)."""
    c = [int(v) for v in coefs]
    out = np.zeros(n, dtype=np.int16)
    h1 = h2 = 0
    for i in range(n):
        f = frames[(i // FRAME) * 8:(i // FRAME) * 8 + 8]
        p, ex = f[0] >> 4, f[0] & 15
        b = f[1 + (i % FRAME) // 2]
        r = (b >> 4) if (i % FRAME) % 2 == 0 else (b & 15)
        r = r - 16 if r >= 8 else r
        s = (((r << ex) << 11) + 1024 + c[2 * p] * h1 + c[2 * p + 1] * h2) >> 11
        s = max(-32768, min(32767, s))
        out[i] = s
        h2, h1 = h1, s
    return out


def envelope(samples):
    """A byte per ENV_BLOCK samples: sqrt(RMS) x 255 (the mouth of a talking person follows it)."""
    n = len(samples)
    blocks = max(1, -(-n // ENV_BLOCK))
    x = np.zeros(blocks * ENV_BLOCK)
    x[:n] = np.asarray(samples, dtype=np.float64) / 32768.0
    rms = np.sqrt((x.reshape(blocks, ENV_BLOCK) ** 2).mean(1))
    return np.clip(np.round(np.sqrt(rms) * 255), 0, 255).astype(np.uint8).tobytes()


def snd3(samples, rate, with_envelope=True):
    """A whole 'SND3' file for int16 samples."""
    coefs, frames = encode(samples)
    env = envelope(samples) if with_envelope else b""
    return (b"SND3" + struct.pack("<II", rate, len(samples)) + coefs.astype("<i2").tobytes()
            + struct.pack("<I", len(env)) + frames + env)


if __name__ == "__main__":
    import argparse
    import time
    import wave

    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("wav", help="16-bit mono WAV")
    ap.add_argument("--out", help="write the decoded sound here")
    args = ap.parse_args()
    with wave.open(args.wav, "rb") as w:
        rate = w.getframerate()
        src = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
        if w.getnchannels() > 1:
            src = src[::w.getnchannels()]
    t = time.time()
    co, fr = encode(src)
    took = time.time() - t
    dec = decode(co, fr, len(src))
    noise = ((src.astype(np.float64) - dec) ** 2).sum()
    snr = 10 * np.log10(max((src.astype(np.float64) ** 2).sum(), 1.0) / max(noise, 1.0))
    print(f"{len(src)} samples, {len(fr)} bytes, encoded in {took * 1000:.0f} ms, SNR {snr:.1f} dB")
    if args.out:
        with wave.open(args.out, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(dec.tobytes())
    sys.exit(0)
