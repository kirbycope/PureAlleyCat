#!/usr/bin/env python3
"""Record what the machine is actually playing and check it against what the game asked for.

    python tools/listen.py --seconds 10 --out capture.wav

The library reports the tone it programs into PIT channel 2, so the expected signal is known
exactly: a square wave at some frequency between roughly 250 Hz and 5 kHz. That makes verification
a measurement rather than a matter of opinion, which is useful because whoever is running this may
not be in a position to listen.

WASAPI loopback captures the output stream directly rather than through a microphone, so there is
no room, no speaker colouring and no background noise to argue with. A microphone would work too
and needs only --device, but everything below is easier to read on a clean signal.

What it looks for:

  * bursts, meaning windows whose energy stands well above the noise floor
  * the fundamental of each burst, by FFT peak
  * odd-harmonic structure, because a square wave carries its 3rd, 5th and 7th harmonics at
    roughly 1/3, 1/5 and 1/7 of the fundamental, and a sine or a click does not

A tone at a plausible frequency with that harmonic signature is the game's speaker and nothing
else on the machine.
"""

from __future__ import annotations

import argparse
import sys
import wave

import numpy as np
import sounddevice as sd

WINDOW = 0.046   # seconds per analysis window; long enough to resolve 250 Hz


def capture(seconds: float, rate: int, device, loopback: bool) -> np.ndarray:
    extra = None
    if loopback:
        try:
            extra = sd.WasapiSettings(loopback=True)
        except (AttributeError, TypeError):
            print("This sounddevice build has no WASAPI loopback; use --mic instead.")
            raise SystemExit(2)
    if device is None:
        device = sd.default.device[1] if loopback else sd.default.device[0]
    info = sd.query_devices(device)
    channels = info["max_output_channels"] if loopback else info["max_input_channels"]
    channels = max(1, min(2, channels))
    print(f"recording {seconds:g}s from [{device}] {info['name']} "
          f"({'loopback' if loopback else 'input'}, {channels}ch @ {rate} Hz)")
    frames = int(seconds * rate)
    audio = sd.rec(frames, samplerate=rate, channels=channels, dtype="float32",
                   device=device, extra_settings=extra)
    sd.wait()
    return audio.mean(axis=1) if audio.ndim > 1 else audio


def harmonic_score(spectrum: np.ndarray, freqs: np.ndarray, fundamental: float) -> float:
    """Ratio of odd-harmonic energy to the fundamental. A square wave sits near 1/3 + 1/5 + 1/7."""
    def at(f):
        if f >= freqs[-1]:
            return 0.0
        return float(spectrum[np.argmin(np.abs(freqs - f))])
    base = at(fundamental)
    if base <= 0:
        return 0.0
    return (at(fundamental * 3) + at(fundamental * 5) + at(fundamental * 7)) / base


def analyse(audio: np.ndarray, rate: int, floor_db: float) -> list[dict]:
    size = int(WINDOW * rate)
    window = np.hanning(size)
    freqs = np.fft.rfftfreq(size, 1.0 / rate)
    energies = []
    frames = []
    for start in range(0, len(audio) - size, size // 2):
        chunk = audio[start:start + size]
        spectrum = np.abs(np.fft.rfft(chunk * window))
        rms = float(np.sqrt(np.mean(chunk ** 2)))
        energies.append(rms)
        frames.append((start / rate, spectrum, rms))

    if not frames:
        return []
    quiet = np.percentile(energies, 20) + 1e-9
    bursts = []
    for when, spectrum, rms in frames:
        if 20 * np.log10(rms / quiet) < floor_db:
            continue
        # ignore the DC end; the speaker never went below ~250 Hz
        usable = freqs > 120
        peak = int(np.argmax(spectrum * usable))
        bursts.append({
            "at": round(when, 3),
            "hz": round(float(freqs[peak]), 1),
            "rms": rms,
            "odd_harmonics": round(harmonic_score(spectrum, freqs, float(freqs[peak])), 3),
        })
    return bursts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--rate", type=int, default=44100)
    parser.add_argument("--device", type=int, default=None)
    parser.add_argument("--mic", action="store_true", help="use an input device instead of loopback")
    parser.add_argument("--floor-db", type=float, default=12.0,
                        help="how far above the quiet baseline counts as a burst")
    parser.add_argument("--out", help="also write the recording to this WAV")
    args = parser.parse_args()

    audio = capture(args.seconds, args.rate, args.device, loopback=not args.mic)

    peak = float(np.max(np.abs(audio)))
    rms = float(np.sqrt(np.mean(audio ** 2)))
    print(f"peak {peak:.4f}  rms {rms:.6f}")
    if peak < 1e-4:
        print("silence: nothing was playing, or the wrong device was captured")
        return 1

    if args.out:
        with wave.open(args.out, "wb") as handle:
            handle.setnchannels(1)
            handle.setsampwidth(2)
            handle.setframerate(args.rate)
            handle.writeframes((np.clip(audio, -1, 1) * 32767).astype("<i2").tobytes())
        print(f"wrote {args.out}")

    bursts = analyse(audio, args.rate, args.floor_db)
    print(f"\n{len(bursts)} windows above the noise floor")
    if not bursts:
        return 1
    print(f"{'time':>8} {'Hz':>8} {'rms':>10} {'odd harmonics':>15}")
    for b in bursts[:40]:
        print(f"{b['at']:>8.3f} {b['hz']:>8.1f} {b['rms']:>10.5f} {b['odd_harmonics']:>15.3f}")
    tones = sorted({b["hz"] for b in bursts})
    square = [b for b in bursts if b["odd_harmonics"] > 0.15]
    print(f"\ndistinct peaks: {len(tones)}, from {tones[0]:.0f} to {tones[-1]:.0f} Hz")
    print(f"windows with square-wave harmonics: {len(square)} of {len(bursts)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
