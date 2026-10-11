"""Live spectrum contract tests; run with a rebuilt native SDK Python."""

import array
import ctypes
import io
import math
import os
from pathlib import Path
import sys
import threading
import time
import unittest
from unittest.mock import patch
import wave

os.environ.setdefault("SDL_AUDIODRIVER", "dummy")
import renpy

if not hasattr(renpy, "audio"):
    renpy.import_all()

from renpy.audio import audio, music, renpysound as sound
from renpy.audio.filter import Multiply


def sine(size=1024, bin_number=16, amplitude=0.5, seconds=2, mode="stereo", rate=48000):
    pcm = array.array("h")
    channels = 1 if mode == "mono" else 2
    for i in range(int(rate * seconds)):
        value = round(32768 * amplitude * math.sin(2 * math.pi * bin_number * i / size))
        if mode == "mono":
            pcm.append(value)
        else:
            pcm.extend((
                0 if mode == "right" else value,
                -value if mode == "opposite" else (0 if mode == "left" else value),
            ))
    data = io.BytesIO()
    with wave.open(data, "wb") as output:
        output.setparams((channels, 2, rate, 0, "NONE", ""))
        output.writeframes(pcm.tobytes())
    return data.getvalue()


class SpectrumAPITests(unittest.TestCase):
    def setUp(self):
        self.channel = audio.Channel("spectrum_test", False, True, False, "", "", True, False, True, False)
        self.patches = [
            patch.dict(audio.channels, {"spectrum_test": self.channel}),
            patch.object(audio, "pcm_ok", False),
        ]
        for p in self.patches:
            p.start()
            self.addCleanup(p.stop)

    def test_validation_without_device(self):
        for size in (0, 511, 513, 8193, 1024.0, True, "4096", None):
            with self.subTest(size=size), self.assertRaises(ValueError):
                music.set_spectrum_enabled(True, "spectrum_test", fft_size=size)
        for rate in (0, 121, float("inf"), float("nan"), "30", True, None, 10**1000):
            with self.subTest(rate=rate), self.assertRaises(ValueError):
                music.set_spectrum_enabled(False, "spectrum_test", update_rate=rate)
        for name in ("unknown", None, [], "audio 99999"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                music.get_spectrum(name)
        self.assertFalse(music.set_spectrum_enabled(True, "spectrum_test"))
        self.assertEqual(self.channel.spectrum_config, (4096, 30.0))
        self.assertIsNone(music.get_spectrum("spectrum_test"))
        self.assertIsNone(self.channel._number)

    def test_disabled_query_and_disable_do_not_allocate(self):
        self.assertIsNone(music.get_spectrum("spectrum_test"))
        with patch.object(audio, "pcm_ok", True), patch.object(sound, "_spectrum_device_available", return_value=True):
            self.assertTrue(music.set_spectrum_enabled(False, "spectrum_test"))
            self.assertTrue(music.set_spectrum_enabled(False, "spectrum_test"))
        self.assertIsNone(self.channel._number)
        self.assertIsNone(self.channel.spectrum_config)

    def test_frame_is_immutable(self):
        frame = audio.SpectrumFrame(1, 1, 48000, 512, None, None, "stopped", (0.0,) * 257, 0.0, 0.0)
        with self.assertRaises(AttributeError):
            frame.rms = 1.0
        with self.assertRaises(TypeError):
            frame.magnitudes[0] = 1.0


class SpectrumRuntimeTests(unittest.TestCase):
    def setUp(self):
        sound.init(48000, True, 256)
        self.assertTrue(sound.set_spectrum_enabled(0, True, 1024, 120))
        self.data = sine()

    def tearDown(self):
        sound.quit()

    def wait(self, predicate, timeout=4):
        deadline = time.monotonic() + timeout
        while True:
            frame = sound.get_spectrum(0)
            if frame is not None and predicate(frame):
                return frame
            self.assertLess(time.monotonic(), deadline, "spectrum condition timed out")
            sound.periodic()
            time.sleep(0.003)

    def play(self, data=None, channel=0, **kwargs):
        sound.play(channel, io.BytesIO(self.data if data is None else data), "tone.wav", end=-1, **kwargs)

    def test_stereo_normalization_and_opposite_phase(self):
        for mode in ("stereo", "opposite", "left", "right", "mono"):
            with self.subTest(mode=mode):
                self.play(sine(mode=mode))
                frame = self.wait(lambda f: f.state == "playing" and f.position > 0.06)
                expected = 0.5 / math.sqrt(2)
                if mode in ("left", "right", "mono"):
                    expected /= math.sqrt(2)
                self.assertEqual(max(range(len(frame.magnitudes)), key=frame.magnitudes.__getitem__), 16)
                self.assertAlmostEqual(frame.magnitudes[16], expected, delta=0.001)
                self.assertAlmostEqual(frame.rms, expected / math.sqrt(2), delta=0.001)
                expected_peak = 0.25 if mode == "mono" else 0.5 / math.sqrt(2)
                self.assertAlmostEqual(frame.peak, expected_peak, delta=0.001)
                self.assertTrue(all(math.isfinite(v) for v in frame.magnitudes))
                self.assertEqual(frame.sample_rate, 48000)
                self.assertEqual(len(frame.magnitudes), 513)

    def test_pause_cache_and_configuration(self):
        self.play()
        frame = self.wait(lambda f: f.state == "playing")
        sound.pause(0)
        frozen = sound.get_spectrum(0)
        self.assertEqual(frozen.state, "paused")
        self.assertEqual(frozen.sequence, frame.sequence)
        self.assertEqual(frozen.magnitudes, frame.magnitudes)
        time.sleep(0.04)
        self.assertIs(sound.get_spectrum(0), frozen)
        self.assertTrue(sound.set_spectrum_enabled(0, True, 1024, 120))
        self.assertIs(sound.get_spectrum(0), frozen)
        sound.unpause(0)
        resumed = self.wait(lambda f: f.sequence > frozen.sequence)
        self.assertEqual(resumed.generation, frozen.generation)
        sound.set_spectrum_enabled(0, True, 2048, 30)
        changed = sound.get_spectrum(0)
        self.assertNotEqual(changed.generation, resumed.generation)
        self.assertEqual(len(changed.magnitudes), 1025)
        sound.set_spectrum_enabled(0, False)
        self.assertIsNone(sound.get_spectrum(0))

    def test_stop_end_seek_queue_and_device(self):
        self.assertEqual(sound.get_spectrum(0).state, "stopped")
        self.play(sine(seconds=0.5))
        playing = self.wait(lambda f: f.state == "playing")
        ended = self.wait(lambda f: f.state == "stopped")
        self.assertNotEqual(ended.generation, playing.generation)
        self.assertEqual((ended.rms, ended.peak), (0, 0))
        self.assertFalse(any(ended.magnitudes))
        self.assertIsNone(ended.sample_time)
        self.assertIsNone(ended.position)
        sound.seek(0, 0.1)
        seeked = sound.get_spectrum(0)
        self.assertNotEqual(seeked.generation, ended.generation)
        self.assertFalse(any(seeked.magnitudes))
        self.wait(lambda f: f.state == "playing" and f.position > 0.15)
        sound.queue(0, io.BytesIO(self.data), "queued.wav", end=-1)
        queued = self.wait(lambda f: f.generation != seeked.generation and f.state == "playing")
        self.assertLess(queued.position, 0.1)
        sound.stop(0)
        stopped = sound.get_spectrum(0)
        self.assertEqual(stopped.state, "stopped")
        self.assertFalse(any(stopped.magnitudes))
        sound.quit()
        self.assertIsNone(sound.get_spectrum(0))
        sound.init(48000, True, 256)
        sound.set_spectrum_enabled(0, True, 1024)
        self.assertNotEqual(sound.get_spectrum(0).generation, stopped.generation)

    def test_gain_pan_mono_output_and_channel_isolation(self):
        self.play(channel=1)
        time.sleep(0.05)
        self.assertFalse(any(sound.get_spectrum(0).magnitudes))
        self.play()
        full = self.wait(lambda f: f.state == "playing" and f.position > 0.08)
        sound.set_volume(0, 0.5)
        half = self.wait(lambda f: f.position > full.position + 0.1)
        self.assertAlmostEqual(half.rms / full.rms, 0.5, delta=0.005)
        sound.set_pan(0, -1, 0)
        left = self.wait(lambda f: f.position > half.position + 0.1)
        self.assertAlmostEqual(left.magnitudes[16], half.magnitudes[16], delta=0.001)
        self.assertAlmostEqual(left.peak, 0.25, delta=0.001)
        sound.set_channel_count(1)
        mono = self.wait(lambda f: f.position > left.position + 0.1)
        self.assertAlmostEqual(mono.rms, left.rms, delta=0.001)
        sound.set_channel_count(2)
        sound.set_volume(0, 0)
        muted = self.wait(lambda f: f.position > mono.position + 0.1)
        self.assertLess(muted.rms, 0.00001)

    def test_seek_start_and_speed_source_coordinates(self):
        self.play(start=0.25, playback_speed=2)
        frame = self.wait(lambda f: f.state == "playing" and f.position > 0.3)
        self.assertAlmostEqual(frame.position, sound.get_pos(0), delta=0.03)
        sound.seek(0, 0.8)
        reset = sound.get_spectrum(0)
        self.assertNotEqual(reset.generation, frame.generation)
        self.assertFalse(any(reset.magnitudes))
        advanced = self.wait(lambda f: f.state == "playing" and f.position > 0.85)
        self.assertAlmostEqual(advanced.position, sound.get_pos(0), delta=0.03)

    def test_relative_volume_filter_fades_and_global_pause(self):
        self.play(relative_volume=0.5, audio_filter=Multiply(4), fadein=0.3)
        early = self.wait(lambda f: f.state == "playing" and f.position > 0.05)
        steady = self.wait(lambda f: f.state == "playing" and f.position > 0.4)
        self.assertGreater(steady.rms, early.rms * 2)
        self.assertAlmostEqual(steady.magnitudes[16], 1 / math.sqrt(2), delta=0.003)
        sound.global_pause(True)
        frozen = sound.get_spectrum(0)
        time.sleep(0.03)
        self.assertEqual(frozen.state, "paused")
        self.assertIs(sound.get_spectrum(0), frozen)
        sound.global_pause(False)
        sound.set_secondary_volume(0, 0.5, 0)
        attenuated = self.wait(lambda f: f.state == "playing" and f.position > steady.position + 0.15)
        self.assertAlmostEqual(attenuated.rms / steady.rms, 0.5, delta=0.003)
        sound.fadeout(0, 0.3)
        fading = self.wait(lambda f: f.state == "playing" and f.position > attenuated.position + 0.15)
        self.assertLess(fading.rms, attenuated.rms * 0.5)
        stopped = self.wait(lambda f: f.state == "stopped")
        self.assertFalse(any(stopped.magnitudes))

    def test_throttle_rate_change_and_capture_restore(self):
        sound.set_spectrum_enabled(0, True, 4096, 1)
        self.play()
        frame = self.wait(lambda f: f.state == "playing" and f.position is not None)
        for _ in range(20):
            self.assertIs(sound.get_spectrum(0), frame)
            time.sleep(0.002)
        sound.set_spectrum_enabled(0, True, 4096, 30)
        changed = sound.get_spectrum(0)
        self.assertNotEqual(changed.generation, frame.generation)
        channel = audio.Channel("spectrum_restore", False, True, False, "", "", True, False, True, False)
        channel._number = 0
        with patch.object(audio, "pcm_ok", True):
            channel.set_spectrum_enabled(True, 4096, 30)
            before = channel.get_spectrum()
            sound.quit()
            self.assertEqual(channel.spectrum_config, (4096, 30.0))
            self.assertIsNone(channel.get_spectrum())
            sound.init(48000, True, 256)
            # init() reinstalls Channel's process-local configuration.
            with (
                patch.object(audio, "all_channels", [channel]),
                patch.object(audio, "periodic_thread", None),
                patch.object(renpy, "emscripten", True),
                patch.object(renpy.config, "webaudio", False),
                patch.object(renpy.game, "preferences", type("Prefs", (), {"volumes": {}, "mute": {}})()),
            ):
                audio.init()
            after = channel.get_spectrum()
            self.assertEqual(after.state, "stopped")
            self.assertNotEqual(after.generation, before.generation)

    def test_unavailable_device_does_not_return_cached_energy(self):
        self.play()
        self.wait(lambda f: f.state == "playing")
        library = Path(sys.executable).with_name("librenpython.dll" if sys.platform == "win32" else "librenpython.so")
        native = ctypes.PyDLL(str(library))
        native.SDL_CloseAudio.argtypes = []
        native.SDL_CloseAudio.restype = None
        native.SDL_CloseAudio()
        self.assertIsNone(sound.get_spectrum(0))
        self.assertIs(sound.set_spectrum_enabled(0, True, 1024, 120), False)
        self.assertIs(sound.set_spectrum_enabled(0, False), False)
        self.assertIsNone(sound.get_spectrum(0))
        channel = audio.Channel("idle_unavailable", False, True, False, "", "", True, False, True, False)
        with patch.object(audio, "pcm_ok", True):
            self.assertFalse(channel.set_spectrum_enabled(False, 4096, 30))
        self.assertIsNone(channel._number)

    def test_concurrent_query_reconfigure_seek_and_quit(self):
        self.play()
        errors = []
        finished = threading.Event()

        def query():
            try:
                while not finished.is_set():
                    frame = sound.get_spectrum(0)
                    if frame is not None:
                        assert len(frame.magnitudes) == frame.fft_size // 2 + 1
                        assert all(math.isfinite(v) for v in frame.magnitudes)
            except BaseException as error:
                errors.append(error)

        threads = [threading.Thread(target=query) for _ in range(3)]
        for thread in threads:
            thread.start()
        try:
            for i in range(60):
                sound.seek(0, 0.1 if i % 2 else 0.5)
                sound.set_spectrum_enabled(0, False)
                sound.set_spectrum_enabled(0, True, 8192 if i % 2 else 512, 120)
                if i % 10 == 0:
                    # Force Channel realloc while copied workspaces are in use.
                    sound.set_volume(100 + i, 1)
                time.sleep(0.001)
            sound.quit()
        finally:
            finished.set()
            for thread in threads:
                thread.join(5)
                self.assertFalse(thread.is_alive())
        self.assertFalse(errors, errors)


if __name__ == "__main__":
    unittest.main()
