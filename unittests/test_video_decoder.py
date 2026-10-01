"""Real FFmpeg movie tests.

Build video_speed_driver.c like audio_tempo_driver.c and set RENPY_VIDEO_DRIVER.
Set RENPY_VIDEO_FIXTURES to a directory containing two-second, 25fps MPEG4 AVI
clips named silent.avi, sound.avi (two-second PCM tone), and short_sound.avi
(half-second PCM tone). They can be generated with ffmpeg's testsrc2 and sine.
"""

import os
from pathlib import Path
import subprocess
import unittest


@unittest.skipUnless(
    os.environ.get("RENPY_VIDEO_DRIVER") and os.environ.get("RENPY_VIDEO_FIXTURES"),
    "native movie driver/fixtures not configured",
)
class VideoDecoderTests(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("RENPY_VIDEO_LIVE_DRIVER"), "live clock driver not configured")
    def test_live_clock_preserves_decoder_and_frame_queue(self):
        fixtures = Path(os.environ["RENPY_VIDEO_FIXTURES"])
        for yuv in (0, 1):
            for dropping in (0, 1):
                with self.subTest(yuv=yuv, dropping=dropping):
                    result = subprocess.run(
                        [
                            os.environ["RENPY_VIDEO_LIVE_DRIVER"],
                            str(fixtures / "silent.avi"),
                            str(fixtures / "sound.avi"),
                            str(yuv),
                            str(dropping),
                        ],
                        capture_output=True,
                        text=True,
                        timeout=40,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                    self.assertIn("PASS:", result.stdout)

    def decode(self, name, speed, yuv, start=0, end=-1, seeks=()):
        path = Path(os.environ["RENPY_VIDEO_FIXTURES"]) / (name + ".avi")
        result = subprocess.run(
            [
                os.environ["RENPY_VIDEO_DRIVER"],
                str(path),
                str(speed),
                str(start),
                str(end),
                str(int(yuv)),
                *map(str, seeks),
            ],
            capture_output=True,
            text=True,
            timeout=40,
        )
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertNotIn("voice tempo", result.stderr)
        rows = [list(map(float, line.split()[1:])) for line in result.stdout.splitlines() if line.startswith("RESULT")]
        self.assertEqual(len(rows), 1 + len(seeks))
        return rows

    def test_pacing_duration_and_silent_tail(self):
        for name in ("silent", "sound", "short_sound"):
            for speed in (0.5, 1, 1.5, 2, 2.5, 3):
                for yuv in (False, True):
                    with self.subTest(name=name, speed=speed, yuv=yuv):
                        [(samples, frames, first, last, position)] = self.decode(name, speed, yuv)
                        self.assertAlmostEqual(
                            samples / 44100, 2 / speed, delta=0.011 if name == "silent" else 1 / 44100
                        )
                        self.assertEqual(frames, 50)
                        self.assertAlmostEqual(last - first, 1.96 / speed, delta=0.011)
                        self.assertAlmostEqual(position, 2, delta=0.001)

    def test_trim_seek_and_seek_after_end(self):
        for name in ("silent", "sound", "short_sound"):
            for speed in (0.5, 1.5, 3):
                for yuv in (False, True):
                    with self.subTest(name=name, speed=speed, yuv=yuv):
                        starts = (0.4, 1.2, 0.2)
                        for start, row in zip(starts, self.decode(name, speed, yuv, starts[0], 1.8, starts[1:])):
                            samples, frames, first, last, position = row
                            self.assertAlmostEqual(
                                samples / 44100, (1.8 - start) / speed, delta=0.011 if name == "silent" else 0.001
                            )
                            self.assertGreater(frames, 0)
                            self.assertLessEqual(last - first, (1.8 - start) / speed + 0.011)
                            self.assertAlmostEqual(position, 1.8, delta=0.001)

    def test_silent_movie_without_duration_metadata(self):
        for speed in (0.5, 1.5, 3):
            with self.subTest(speed=speed):
                [(samples, frames, first, last, position)] = self.decode("silent", speed, 3)
                self.assertEqual(frames, 50)
                self.assertAlmostEqual(last - first, 1.96 / speed, delta=0.011)
                self.assertAlmostEqual(samples / 44100, 1.96 / speed, delta=0.011)
                self.assertAlmostEqual(position, 1.96, delta=0.031)
