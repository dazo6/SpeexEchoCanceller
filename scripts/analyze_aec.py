#!/usr/bin/env python3
"""Calculate reproducible level and far-end leakage proxies for AEC WAV files."""

from __future__ import annotations

import argparse
import math
import wave
from pathlib import Path

import numpy as np


def read_pcm16(path: Path) -> tuple[int, np.ndarray]:
    with wave.open(str(path), "rb") as wav:
        if wav.getnchannels() != 1 or wav.getsampwidth() != 2:
            raise ValueError(f"{path}: expected mono PCM16 WAV")
        rate = wav.getframerate()
        samples = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2")
    return rate, samples.astype(np.float64) / 32768.0


def db(value: float, floor: float = 1e-12) -> float:
    return 20.0 * math.log10(max(abs(value), floor))


def rms(signal: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(signal))))


def best_lag(signal: np.ndarray, reference: np.ndarray, max_lag: int) -> int:
    size = len(signal) + len(reference) - 1
    fft_size = 1 << (size - 1).bit_length()
    convolution = np.fft.irfft(
        np.fft.rfft(signal, fft_size) * np.fft.rfft(reference[::-1], fft_size), fft_size
    )[:size]
    lags = np.arange(-(len(reference) - 1), len(signal))
    selected = (lags >= -max_lag) & (lags <= max_lag)
    return int(lags[selected][np.argmax(np.abs(convolution[selected]))])


def align(signal: np.ndarray, reference: np.ndarray, lag: int) -> tuple[np.ndarray, np.ndarray]:
    if lag >= 0:
        length = min(len(signal) - lag, len(reference))
        return signal[lag : lag + length], reference[:length]
    length = min(len(signal), len(reference) + lag)
    return signal[:length], reference[-lag : -lag + length]


def metrics(signal: np.ndarray, reference: np.ndarray) -> dict[str, float]:
    signal = signal - np.mean(signal)
    reference = reference - np.mean(reference)
    denominator = float(np.dot(reference, reference)) + 1e-18
    coefficient = float(np.dot(signal, reference) / denominator)
    projected = coefficient * reference
    correlation = float(np.corrcoef(signal, reference)[0, 1])
    return {
        "rms_dbfs": db(rms(signal)),
        "peak_dbfs": db(float(np.max(np.abs(signal)))),
        "far_end_corr": abs(correlation),
        "projected_echo_dbfs": db(rms(projected)),
        "projection_coefficient": coefficient,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mic", type=Path, required=True)
    parser.add_argument("--loopback", type=Path, required=True)
    parser.add_argument("outputs", nargs="+", type=Path)
    args = parser.parse_args()

    rate, mic = read_pcm16(args.mic)
    ref_rate, reference = read_pcm16(args.loopback)
    if rate != ref_rate:
        raise ValueError("sample rates differ")
    lag = best_lag(mic, reference, rate)
    aligned_mic, aligned_reference = align(mic, reference, lag)
    baseline = metrics(aligned_mic, aligned_reference)

    print(f"alignment_lag_samples={lag}, alignment_lag_ms={lag * 1000 / rate:.3f}")
    print("name,rms_dbfs,level_vs_mic_db,peak_dbfs,far_end_corr,projected_echo_dbfs,echo_projection_reduction_db")
    for path in args.outputs:
        output_rate, output = read_pcm16(path)
        if output_rate != rate:
            raise ValueError(f"{path}: sample rate differs")
        aligned_output, output_reference = align(output, reference, lag)
        length = min(len(aligned_output), len(aligned_mic), len(output_reference))
        result = metrics(aligned_output[:length], output_reference[:length])
        mic_level = rms(aligned_mic[:length] - np.mean(aligned_mic[:length]))
        output_level = rms(aligned_output[:length] - np.mean(aligned_output[:length]))
        reduction = baseline["projected_echo_dbfs"] - result["projected_echo_dbfs"]
        print(
            f"{path.stem},{result['rms_dbfs']:.3f},{db(output_level / mic_level):.3f},"
            f"{result['peak_dbfs']:.3f},{result['far_end_corr']:.5f},"
            f"{result['projected_echo_dbfs']:.3f},{reduction:.3f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
