"""Native mixer/movie integration, using fixtures described in test_video_decoder."""

import os
from pathlib import Path
import time
import unittest
import io
import threading

os.environ.setdefault("SDL_AUDIODRIVER", "dummy")
import renpy

if not hasattr(renpy, "audio"):
    renpy.import_all()
from renpy.audio import renpysound as sound


@unittest.skipUnless(os.environ.get("RENPY_VIDEO_FIXTURES"), "movie fixtures not configured")
class VideoRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sound.init(44100, True, 441)
        sound.set_video(0, sound.DROP_VIDEO)

    @classmethod
    def tearDownClass(cls):
        sound.quit()

    def tearDown(self):
        sound.global_pause(False)
        sound.unpause(0)
        sound.stop(0)
        sound.stop(1)
        sound.periodic()

    def wait_for(self, condition, timeout=15):
        # Windows' dummy audio driver can run slower than real time because
        # of timer granularity. Speed assertions compare with a 1x baseline.
        deadline = time.monotonic() + timeout
        while not condition():
            self.assertLess(
                time.monotonic(), deadline, f"movie condition timed out at source position {sound.get_pos(0)}"
            )
            sound.periodic()
            if sound.video_ready(0):
                sound.read_video_yuv(0)
            time.sleep(0.005)

    def play(self, name, speed, queue=False, channel=0, label=None, live_speed=None):
        path = Path(os.environ["RENPY_VIDEO_FIXTURES"]) / (name + ".avi")
        with path.open("rb") as f:
            import io

            data = io.BytesIO(f.read())
        (sound.queue if queue else sound.play)(
            channel, data, label or path.name, end=-1, playback_speed=speed, video_playback_speed=live_speed
        )

    def assert_rate(self, expected, duration=0.2):
        pos = sound.get_pos(0)
        start = time.monotonic()
        self.wait_for(lambda: time.monotonic() - start >= duration)
        elapsed = time.monotonic() - start
        self.assertAlmostEqual(sound.get_pos(0) - pos, elapsed * expected, delta=0.025)

    def test_live_continuity_source_clock_and_isolation(self):
        sound.set_video(1, sound.DROP_VIDEO)
        self.play("silent", 1)
        self.play("silent", 1, channel=1)
        self.wait_for(lambda: sound.get_pos(0) > 0.15)
        before = sound.get_pos(0)
        sound.set_playback_speed(0, 0.5)
        self.assertAlmostEqual(sound.get_pos(0), before, delta=0.02)
        self.assertEqual(sound.get_playback_speed(0), 0.5)
        self.assertEqual(sound.get_playback_speed(1), 1)
        other_pos = sound.get_pos(1)
        start = time.monotonic()
        self.assert_rate(0.5, duration=1)
        self.assertAlmostEqual(sound.get_pos(1) - other_pos, time.monotonic() - start, delta=0.03)
        for _ in range(50):
            sound.set_playback_speed(0, 0.5)
        before = sound.get_pos(0)
        sound.set_playback_speed(0, 1)
        self.assertAlmostEqual(sound.get_pos(0), before, delta=0.02)
        self.assert_rate(1)
        self.assertEqual(sound.get_duration(0), 2)

    def test_predecoded_and_future_loops_keep_latest_rate(self):
        self.play("silent", 1)
        self.play("silent", 1, queue=True, label="loop2")
        self.wait_for(lambda: sound.get_pos(0) >= 1.7)
        before = sound.get_pos(0)
        sound.set_playback_speed(0, 0.5)
        self.assertAlmostEqual(sound.get_pos(0), before, delta=0.02)
        self.wait_for(lambda: sound.playing_name(0) == "loop2")
        self.assertEqual(sound.get_playback_speed(0), 0.5)
        self.assert_rate(0.5)
        # Channel.periodic passes the latest request before starting a new
        # decoder, independently of each queue entry's original audio tempo.
        self.play("silent", 1, queue=True, label="loop3", live_speed=0.5)
        self.wait_for(lambda: sound.playing_name(0) == "loop3")
        self.assertEqual(sound.get_playback_speed(0), 0.5)
        self.assert_rate(0.5)

    def test_pending_pause_resume_and_reset(self):
        sound.global_pause(True)
        self.play("silent", 1)
        sound.set_playback_speed(0, 0.5)
        time.sleep(0.05)
        self.assertEqual(sound.get_pos(0), 0)
        self.assertFalse(sound.video_ready(0))
        sound.global_pause(False)
        self.wait_for(lambda: sound.get_pos(0) > 0.05)
        sound.pause(0)
        pos = sound.get_pos(0)
        sound.set_playback_speed(0, 3)
        sound.global_pause(True)
        time.sleep(0.05)
        sound.global_pause(False)
        time.sleep(0.05)
        self.assertEqual(sound.get_pos(0), pos)
        self.assertEqual(sound.get_playback_speed(0), 3)
        sound.unpause(0)
        self.assert_rate(3)
        sound.stop(0)
        self.assertEqual(sound.get_playback_speed(0), 1)
        self.play("silent", 1.5)
        self.assertEqual(sound.get_playback_speed(0), 1.5)
        self.wait_for(lambda: sound.get_pos(0) > 0.05)
        self.assert_rate(1.5)

    def test_request_before_metadata_and_first_frame(self):
        gate = threading.Event()

        class GatedFile(io.BytesIO):
            def read(self, size=-1):
                if not gate.wait(10):
                    raise TimeoutError("decoder was not released")
                return super().read(size)

        path = Path(os.environ["RENPY_VIDEO_FIXTURES"]) / "silent.avi"
        data = GatedFile(path.read_bytes())
        try:
            sound.play(0, data, "pending.avi", end=-1)
            sound.set_playback_speed(0, 0.5)
            self.assertEqual(sound.get_playback_speed(0), 0.5)
            self.assertEqual(sound.get_pos(0), 0)
        finally:
            gate.set()
        self.wait_for(lambda: sound.get_pos(0) > 0.05)
        self.assert_rate(0.5)

    def test_audio_track_rejected_without_changing_request(self):
        self.play("sound", 1)
        self.wait_for(lambda: sound.get_duration(0) > 0)
        with self.assertRaises(ValueError):
            sound.set_playback_speed(0, 0.5)
        self.assertEqual(sound.get_playback_speed(0), 1)

    def test_silent_movie_speed_and_source_clock(self):
        durations = []
        for speed in (1, 2, 0.5):
            self.play("silent", speed)
            self.wait_for(lambda: sound.get_pos(0) > 0.05)
            start = time.monotonic()
            self.wait_for(lambda: sound.playing_at_end(0))
            durations.append(time.monotonic() - start)
            self.assertAlmostEqual(sound.get_pos(0), 2, delta=0.02)
            self.assertAlmostEqual(sound.get_duration(0), 2, delta=0.02)
        self.assertAlmostEqual(durations[1] / durations[0], 0.5, delta=0.12)
        self.assertAlmostEqual(durations[2] / durations[0], 2, delta=0.2)

    def test_pause_seek_and_queued_speed(self):
        self.play("short_sound", 3)
        self.play("silent", 0.5, queue=True)
        self.wait_for(lambda: sound.get_pos(0) > 0.1)
        sound.pause(0)
        position = sound.get_pos(0)
        time.sleep(0.06)
        self.assertAlmostEqual(sound.get_pos(0), position, delta=0.01)
        sound.unpause(0)
        self.wait_for(lambda: sound.playing_name(0) == "silent.avi")
        self.wait_for(lambda: sound.get_pos(0) > 0.05)
        position = sound.get_pos(0)
        time.sleep(0.12)
        self.assertLess(sound.get_pos(0) - position, 0.12)
        sound.seek(0, 1.8)
        self.wait_for(lambda: sound.playing_at_end(0))
        sound.seek(0, 1.5)
        self.assertFalse(sound.playing_at_end(0))
        self.wait_for(lambda: sound.get_pos(0) > 1.6)
        self.wait_for(lambda: sound.playing_at_end(0))
        self.assertAlmostEqual(sound.get_pos(0), 2, delta=0.02)
