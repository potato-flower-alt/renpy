"""Real FFmpeg decoder tests. Set RENPY_TEMPO_DRIVER to audio_tempo_driver.

Build the driver using the same SDL2/FFmpeg libraries as the engine, e.g.:
cc unittests/audio_tempo_driver.c -o /tmp/audio_tempo_driver \
    $(pkg-config --cflags --libs --static libavfilter libavformat libavcodec \
      libswresample libswscale sdl2) -lm -pthread
"""

import array
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest
import wave


@unittest.skipUnless(os.environ.get("RENPY_TEMPO_DRIVER"), "native decoder driver not built")
class AudioTempoTests(unittest.TestCase):
    speeds = (0.5, 1.0, 1.5, 2.0, 2.5, 3.0)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)

    def tone(self, duration=2.0, rate=44100, channels=2):
        path = Path(self.temp.name) / "tone.wav"
        samples = array.array("h")
        for i in range(round(duration * rate)):
            for channel in range(channels):
                samples.append(round(12000 * math.sin(2 * math.pi * (440 + channel * 220) * i / rate)))
        with wave.open(str(path), "wb") as output:
            output.setparams((channels, 2, rate, 0, "NONE", "not compressed"))
            output.writeframes(samples.tobytes())
        return path

    def decode(self, path, speed, start=0, end=-1, seeks=()):
        result = subprocess.run(
            [os.environ["RENPY_TEMPO_DRIVER"], str(path), str(speed), str(start), str(end), *map(str, seeks)],
            capture_output=True,
            timeout=40,
        )
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        metadata = [tuple(map(float, m)) for m in re.findall(rb"RESULT (\d+) ([\d.]+) ([\d.]+)", result.stderr)]
        self.assertNotIn(b"[ffmedia] voice tempo", result.stderr, result.stderr.decode(errors="replace"))
        self.assertEqual(len(metadata), 1 + len(seeks), result.stderr.decode(errors="replace"))
        pcm = array.array("h")
        pcm.frombytes(result.stdout)
        return pcm, metadata

    def wav24(self, samples, channels=2, rate=44100, extensible=False):
        """Write packed signed PCM, including padding and an unknown RIFF chunk."""
        path = Path(self.temp.name) / "pcm24.wav"
        fmt = struct.pack("<HHIIHH", 0xFFFE if extensible else 1, channels, rate, rate * channels * 3, channels * 3, 24)
        if extensible:
            # PCM subtype GUID; leave mono layout unspecified, like classic WAV.
            fmt += struct.pack("<HHI", 22, 24, 3 if channels == 2 else 0)
            fmt += bytes.fromhex("01000000 0000 1000 8000 00aa00389b71")
        data = b"".join(sample.to_bytes(3, "little", signed=True) for sample in samples)

        def chunk(tag, data):
            return tag + struct.pack("<I", len(data)) + data + b"\0" * (len(data) % 2)

        body = b"WAVE" + chunk(b"fmt ", fmt) + chunk(b"JUNK", b"odd") + chunk(b"data", data)
        path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)
        return path

    def test_pcm24_sample_values(self):
        # Asymmetric stereo catches interleaving errors. Include signed limits,
        # silence, and sub-16-bit values to check the existing S32 -> S16 path.
        values = (-8388608, 8388607, 0, -1, 1, -256, 256, -4194304, 4194304)
        for channels in (1, 2):
            source = [values[i % len(values)] for i in range(257 * channels)]
            expected = [max(-32768, min(32767, value >> 8)) for value in source]
            if channels == 1:
                expected = [value for value in expected for _ in range(2)]
            for extensible in (False, True):
                with self.subTest(channels=channels, extensible=extensible):
                    path = self.wav24(source, channels=channels, extensible=extensible)
                    pcm, [(samples, _, duration)] = self.decode(path, 1)
                    self.assertEqual(samples, 257)
                    self.assertEqual(len(pcm), len(expected))
                    if channels == 1:
                        # Mono mixing uses floating point and can round instead
                        # of truncating sub-16-bit values. Allow one output LSB.
                        self.assertLessEqual(max(abs(a - b) for a, b in zip(pcm, expected)), 1)
                        self.assertEqual(pcm[::2], pcm[1::2])
                    else:
                        self.assertEqual(list(pcm), expected)
                    self.assertAlmostEqual(duration, 257 / 44100, delta=0.000001)

    def test_pcm24_resampling_and_tempo(self):
        for rate, channels in ((16000, 1), (44100, 2), (48000, 2)):
            reference = self.tone(rate=rate, channels=channels)
            with wave.open(str(reference)) as source:
                samples = array.array("h", source.readframes(source.getnframes()))
            path = self.wav24((sample * 256 for sample in samples), channels=channels, rate=rate)
            for speed in (1.0, 1.5, 3.0):
                with self.subTest(rate=rate, channels=channels, speed=speed):
                    expected, expected_metadata = self.decode(reference, speed)
                    pcm, metadata = self.decode(path, speed)
                    self.assertEqual(pcm, expected)
                    self.assertEqual(metadata, expected_metadata)

    def test_pcm24_crop_and_repeated_seek(self):
        reference = self.tone()
        with wave.open(str(reference)) as source:
            samples = array.array("h", source.readframes(source.getnframes()))
        path = self.wav24((sample * 256 for sample in samples), extensible=True)
        for speed in (1.0, 1.5):
            with self.subTest(speed=speed):
                pcm, passes = self.decode(path, speed, 0.25, 1.75, (1.25, 0.5, 1.75))
                self.assertTrue(pcm)
                for (samples, position, _), start in zip(passes, (0.25, 1.25, 0.5, 1.75)):
                    self.assert_duration(samples, 1.75 - start, speed)
                    self.assertAlmostEqual(position, 1.75, delta=0.025)

    def assert_duration(self, samples, source_duration, speed):
        self.assertAlmostEqual(samples / 44100, source_duration / speed, delta=0.1)

    def test_empty_reads_leave_decoder_idle(self):
        result = subprocess.run(
            [os.environ["RENPY_TEMPO_DRIVER"], "--idle"], capture_output=True, timeout=5
        )
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))

    def test_pitch_duration_and_resampling(self):
        for rate, channels in ((16000, 1), (44100, 2), (48000, 2)):
            path = self.tone(rate=rate, channels=channels)
            for speed in self.speeds:
                with self.subTest(rate=rate, channels=channels, speed=speed):
                    pcm, [(samples, position, duration)] = self.decode(path, speed)
                    self.assert_duration(samples, 2, speed)
                    self.assertAlmostEqual(position, 2, delta=0.02)
                    self.assertAlmostEqual(duration, 2, delta=0.02)
                    for channel in (0, 1):
                        signal = pcm[channel::2][2000:-2000]
                        crossings = sum(a <= 0 < b for a, b in zip(signal, signal[1:]))
                        frequency = crossings * 44100 / len(signal)
                        expected = 440 + (channel * 220 if channels == 2 else 0)
                        self.assertAlmostEqual(frequency, expected, delta=expected * 0.01)

                        # Compare spectral energy at the original pitch and at
                        # nearby frequencies (Goertzel, no test dependency).
                        def energy(frequency):
                            coefficient = 2 * math.cos(2 * math.pi * frequency / 44100)
                            a = b = 0.0
                            for sample in signal[:8192]:
                                a, b = sample + coefficient * a - b, a
                            return a * a + b * b - coefficient * a * b

                        peak = energy(expected)
                        self.assertGreater(peak, 10 * max(energy(expected - 25), energy(expected + 25)))

    def test_unity_is_bit_exact(self):
        path = self.tone()
        pcm, _ = self.decode(path, 1)
        with wave.open(str(path)) as source:
            self.assertEqual(pcm.tobytes(), source.readframes(source.getnframes()))

    def test_crop_and_repeated_seek_after_eof(self):
        path = self.tone()
        for speed in self.speeds:
            with self.subTest(speed=speed):
                _, passes = self.decode(path, speed, 0.25, 1.75, (1.25, 0.5, 1.75))
                for (samples, position, _), start in zip(passes, (0.25, 1.25, 0.5, 1.75)):
                    self.assert_duration(samples, 1.75 - start, speed)
                    self.assertAlmostEqual(position, 1.75, delta=0.025)

    def test_short_clips_and_explicit_silence(self):
        for seconds in (0.005, 0.02, 0.1):
            path = self.tone(duration=seconds)
            for speed in self.speeds:
                with self.subTest(seconds=seconds, speed=speed):
                    pcm, [(samples, _, _)] = self.decode(path, speed)
                    self.assertGreater(samples, 0)
                    self.assertGreater(max(map(abs, pcm)), 1000)
                    self.assert_duration(samples, seconds, speed)

        path = self.tone(duration=0.1)
        for speed in self.speeds:
            _, [(samples, position, _)] = self.decode(path, speed, end=1.0)
            self.assert_duration(samples, 1, speed)
            self.assertAlmostEqual(position, 1, delta=0.025)

    def test_compressed_audio(self):
        root = Path(__file__).resolve().parents[1]
        for path in (root / "testcases/game/sound/1.ogg", root / "the_question/game/illurock.opus"):
            for speed in self.speeds:
                with self.subTest(file=path.name, speed=speed):
                    pcm, [(samples, position, _)] = self.decode(path, speed, end=2.0)
                    self.assert_duration(samples, 2, speed)
                    self.assertAlmostEqual(position, 2, delta=0.025)
                    self.assertGreater(max(map(abs, pcm)), 100)

    def test_mp3_without_duration_metadata(self):
        path = Path(__file__).parent / "audio/voice-tone.mp3"
        _, [(baseline, _, duration)] = self.decode(path, 1)
        self.assertEqual(duration, 0)
        for speed in self.speeds:
            pcm, [(samples, position, duration)] = self.decode(path, speed)
            self.assert_duration(samples, baseline / 44100, speed)
            self.assertAlmostEqual(position, baseline / 44100, delta=0.025)
            self.assertEqual(duration, 0)
            self.assertGreater(max(map(abs, pcm)), 1000)

    def test_silence_and_invalid_input_finish(self):
        silence = Path(__file__).resolve().parents[1] / "renpy/common/_silence.ogg"
        for speed in self.speeds:
            pcm, [(samples, position, _)] = self.decode(silence, speed, end=0.75)
            self.assert_duration(samples, 0.75, speed)
            self.assertAlmostEqual(position, 0.75, delta=0.025)
            self.assertEqual(max(map(abs, pcm)), 0)

        path = Path(self.temp.name) / "broken.wav"
        path.write_bytes(b"not an audio file")
        for speed in (1, 1.5, 3):
            pcm, [(samples, _, _)] = self.decode(path, speed)
            self.assertEqual(samples, 0)
            self.assertFalse(pcm)


if __name__ == "__main__":
    unittest.main()
