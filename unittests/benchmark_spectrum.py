"""Query/FFT/copy benchmark using a rebuilt native SDK Python and dummy audio.

The forced native copy timing includes FFI and waiting for the audio lock, so
its observed maximum conservatively bounds the copy's lock-held duration.
This measures decoder starvation; it cannot report hardware device underruns.
"""

import ctypes
import io
import json
import os
from pathlib import Path
import statistics
import sys
import time

os.environ.setdefault("SDL_AUDIODRIVER", "dummy")

from unittests.test_spectrum import sine, sound


class Snapshot(ctypes.Structure):
    _fields_ = [
        ("generation", ctypes.c_uint64),
        ("count", ctypes.c_uint64),
        ("sample_rate", ctypes.c_int),
        ("fft_size", ctypes.c_int),
        ("state", ctypes.c_int),
        ("sample_time", ctypes.c_double),
        ("position", ctypes.c_double),
    ]


def distribution(values):
    values = sorted(values)
    return {
        "calls": len(values),
        "median_us": statistics.median(values) / 1000,
        "p99_us": values[int((len(values) - 1) * 0.99)] / 1000,
        "max_us": max(values) / 1000,
    }


def main():
    size = 4096
    sound.init(48000, True, 512)
    data = sine(size=size, seconds=12)
    sound.play(0, io.BytesIO(data), "bench.wav", end=-1)
    try:
        sound.set_spectrum_enabled(0, True, size, 30)
        deadline = time.perf_counter() + 3
        while True:
            frame = sound.get_spectrum(0)
            if frame is not None and frame.state == "playing" and frame.position > 0.1:
                break
            if time.perf_counter() > deadline:
                raise RuntimeError("Audio did not warm up.")
            time.sleep(0.002)
        # Observe normal 30 Hz output-sample throttling over wall time.
        analysis, cached, starved = [], [], 0
        previous = frame
        started = time.perf_counter()
        deadline = started + 3
        cpu_started = time.thread_time_ns()
        while time.perf_counter() < deadline:
            tick = time.perf_counter_ns()
            frame = sound.get_spectrum(0)
            elapsed = time.perf_counter_ns() - tick
            if frame is not None:
                (cached if frame.sequence == previous.sequence else analysis).append(elapsed)
                starved += frame.state == "starved"
                previous = frame
            time.sleep(0.002)
        elapsed = time.perf_counter() - started
        thread_cpu = time.thread_time_ns() - cpu_started
        library = Path(sys.executable).with_name("librenpython.dll" if sys.platform == "win32" else "librenpython.so")
        native = ctypes.PyDLL(str(library))  # Retain the GIL, like Cython queries.
        copy = native.RPS_copy_spectrum
        copy.argtypes = [
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_uint64,
            ctypes.c_uint64,
            ctypes.c_int,
            ctypes.c_double,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(Snapshot),
        ]
        copy.restype = ctypes.c_int
        pcm = (ctypes.c_float * (2 * size))()
        snapshot = Snapshot()
        copies = []
        for _ in range(10000):
            tick = time.perf_counter_ns()
            assert copy(0, size, 0, 0, -1, 30, pcm, ctypes.byref(snapshot)) == 2
            copies.append(time.perf_counter_ns() - tick)
        report = {
            "platform": sys.platform,
            "driver": os.environ["SDL_AUDIODRIVER"],
            "fft_size": size,
            "output_sample_rate": frame.sample_rate,
            "configured_update_rate": 30,
            "wall_seconds": elapsed,
            "polling_thread_cpu_ms": thread_cpu / 1e6,
            "polling_thread_cpu_percent": thread_cpu / (elapsed * 1e7),
            "query_elapsed_ms": (sum(analysis) + sum(cached)) / 1e6,
            "new_analysis": distribution(analysis),
            "cached_query": distribution(cached),
            "native_copy_including_ffi_and_lock_wait": distribution(copies),
            "starved_snapshots_after_warmup": starved,
            "native_buffer_bytes_excluding_structs": 2 * size * 4 * 2 + (size // 2 + 1) * 8 + size * 4 + 6 * size * 8,
            "snapshot_tuple_and_magnitudes_bytes": sys.getsizeof(frame)
            + sys.getsizeof(frame.magnitudes)
            + sum(sys.getsizeof(v) for v in frame.magnitudes),
            "hardware_underruns": "not measurable with the dummy driver",
        }
        destination = Path(sys.argv[1]) if len(sys.argv) > 1 else None
        result = json.dumps(report, indent=2)
        if destination:
            destination.write_text(result, encoding="utf-8")
        print(result)
    finally:
        sound.quit()


if __name__ == "__main__":
    main()
