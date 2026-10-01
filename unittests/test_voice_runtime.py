"""Mixer/decoder integration tests; run with a rebuilt native SDK Python."""

import array
import io
import math
import os
import time
import unittest
import wave

os.environ.setdefault("SDL_AUDIODRIVER", "dummy")
import renpy

if not hasattr(renpy, "audio"):
    renpy.import_all()
from renpy.audio import renpysound as sound


class VoiceRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sound.init(44100, True, 256)
        data = io.BytesIO()
        with wave.open(data, "wb") as output:
            output.setparams((1, 2, 44100, 0, "NONE", ""))
            output.writeframes(
                array.array(
                    "h", (round(12000 * math.sin(2 * math.pi * 440 * i / 44100)) for i in range(22050))
                ).tobytes()
            )
        cls.data = data.getvalue()

    @classmethod
    def tearDownClass(cls):
        sound.quit()

    def tearDown(self):
        sound.stop(0)
        sound.periodic()

    def wait_for(self, condition, timeout=3):
        deadline = time.monotonic() + timeout
        while not condition():
            self.assertLess(time.monotonic(), deadline, "audio condition timed out")
            sound.periodic()
            time.sleep(0.005)

    def play(self, speed, name="voice.wav"):
        sound.play(0, io.BytesIO(self.data), name, end=-1, playback_speed=speed)

    def test_pause_resume_and_source_clock(self):
        self.play(2)
        self.wait_for(lambda: sound.get_pos(0) > 0.1)
        sound.pause(0)
        position = sound.get_pos(0)
        time.sleep(0.04)
        self.assertAlmostEqual(sound.get_pos(0), position, delta=0.02)
        sound.unpause(0)
        self.wait_for(lambda: sound.playing_at_end(0))
        self.assertAlmostEqual(sound.get_pos(0), 0.5, delta=0.025)
        self.assertAlmostEqual(sound.get_duration(0), 0.5, delta=0.025)

    def test_queue_keeps_independent_speed(self):
        # The dummy driver's clock depends on OS timer granularity. Calibrate
        # against the same clip at 1x; the offline decoder tests sample counts.
        self.play(1)
        self.wait_for(lambda: sound.get_pos(0) > 0)
        start = time.monotonic()
        self.wait_for(lambda: sound.playing_at_end(0))
        normal_duration = time.monotonic() - start

        self.play(3, "first.wav")
        sound.queue(0, io.BytesIO(self.data), "second.wav", end=-1, playback_speed=0.5)
        self.wait_for(lambda: sound.get_pos(0) > 0)
        start = time.monotonic()
        self.wait_for(lambda: sound.playing_name(0) == "second.wav")
        self.assertAlmostEqual(time.monotonic() - start, normal_duration / 3, delta=normal_duration * 0.2)
        start = time.monotonic()
        self.wait_for(lambda: sound.playing_at_end(0))
        elapsed = time.monotonic() - start
        self.assertAlmostEqual(elapsed, normal_duration * 2, delta=normal_duration * 0.3)
        self.assertAlmostEqual(sound.get_pos(0), 0.5, delta=0.025)

    def test_seek_during_playback_and_after_end(self):
        self.play(1.5)
        self.wait_for(lambda: sound.get_pos(0) > 0.03)
        for target in (0.3, 0.1, 0.4):
            sound.seek(0, target)
            self.assertFalse(sound.playing_at_end(0))
            self.wait_for(lambda: sound.get_pos(0) > target + 0.02)
        self.wait_for(lambda: sound.playing_at_end(0))
        sound.seek(0, 0.1)
        self.assertFalse(sound.playing_at_end(0))
        self.wait_for(lambda: sound.get_pos(0) > 0.15)
        self.wait_for(lambda: sound.playing_at_end(0))
        self.assertAlmostEqual(sound.get_pos(0), 0.5, delta=0.025)

    def test_queue_after_natural_end(self):
        for speed in (1.0, 1.5, 3.0):
            with self.subTest(speed=speed):
                self.play(speed)
                self.wait_for(lambda: sound.playing_at_end(0))
                position = sound.get_pos(0)
                time.sleep(0.04)
                self.assertEqual(sound.get_pos(0), position)
                # The completed stream stays open for seeking, but a new
                # queued file must also be able to wake the idle channel.
                sound.queue(0, io.BytesIO(self.data), "next.wav", end=-1, playback_speed=1.5)
                self.wait_for(lambda: sound.playing_name(0) == "next.wav")
                self.assertFalse(sound.playing_at_end(0))
                self.wait_for(lambda: sound.playing_at_end(0))
                self.assertAlmostEqual(sound.get_pos(0), 0.5, delta=0.025)

    def test_stop_during_decode_and_fade(self):
        for speed in (0.5, 3, 2, 1):
            self.play(speed)
            sound.stop(0)
            self.assertIsNone(sound.playing_name(0))
        self.play(0.5)
        self.wait_for(lambda: sound.get_pos(0) > 0.01)
        sound.fadeout(0, 0.05)
        self.wait_for(lambda: sound.playing_at_end(0))
        self.assertLess(sound.get_pos(0), 0.2)


if __name__ == "__main__":
    unittest.main()
