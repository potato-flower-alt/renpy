"""Run with the SDK Python: python -m unittest unittests.test_voice_speed."""

import pickle
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import renpy

if not hasattr(renpy, "audio"):
    renpy.import_all()

from renpy.audio.audio import Channel
from renpy.exports.mediaexports import get_voice_speed, set_voice_speed
from renpy.preferences import Preferences, validate_voice_speed


class VoiceSpeedTests(unittest.TestCase):
    def setUp(self):
        self.preferences = Preferences()
        self.patches = [
            patch.object(renpy.game, "preferences", self.preferences),
            patch.object(renpy.game, "context", return_value=SimpleNamespace(music={})),
            patch.object(renpy.game, "log", SimpleNamespace(mutated={})),
            patch.object(renpy.exports, "is_seen_allowed", return_value=False),
            patch.object(renpy, "emscripten", False),
        ]
        for p in self.patches:
            p.start()
            self.addCleanup(p.stop)

    def channel(self, name="voice", movie=False):
        return Channel(name, False, True, False, "", "", True, movie, True, False)

    def test_validation_and_api(self):
        self.assertEqual(get_voice_speed(), 1.0)
        for speed in (0.5, 1, 1.234, 2, 2.5, 3):
            set_voice_speed(speed)
            self.assertEqual(get_voice_speed(), float(speed))
        for value in (True, False, None, "1.5", [], {}):
            with self.subTest(value=value), self.assertRaises(TypeError):
                set_voice_speed(value)
        for value in (0, -1, 0.49, 3.01, float("nan"), float("inf"), -float("inf"), 10**1000):
            with self.subTest(value=value), self.assertRaises(ValueError):
                validate_voice_speed(value)

    def test_persistence_upgrade_and_reset(self):
        set_voice_speed(2.5)
        loaded = pickle.loads(pickle.dumps(self.preferences))
        self.assertEqual(loaded.voice_speed, 2.5)
        del loaded.voice_speed
        loaded.after_upgrade(0)
        self.assertEqual(loaded.voice_speed, 1.0)
        loaded.voice_speed = 3.0
        loaded.reset()
        self.assertEqual(loaded.voice_speed, 1.0)

    def test_damaged_preferences(self):
        for value in (0, 4, True, float("nan"), float("inf"), "bad"):
            with self.subTest(value=value):
                self.preferences.voice_speed = value
                self.assertIsNotNone(self.preferences.check())
                self.assertEqual(self.preferences.voice_speed, 1.0)

    def test_queue_captures_each_request(self):
        channel = self.channel()
        set_voice_speed(1.5)
        channel.enqueue(["a.ogg", "b.ogg"], loop=False)
        set_voice_speed(3.0)
        channel.enqueue(["c.ogg"], loop=False)
        self.assertEqual([q.playback_speed for q in channel.queue], [1.5, 1.5, 3.0])
        self.assertIsNone(channel._number)  # No native device is needed to set a preference.

    def test_voice_only_and_web_bypass(self):
        set_voice_speed(2)
        self.assertEqual(self.channel().get_playback_speed(), 2)
        for name in ("music", "sound", "movie", "custom_voice"):
            self.assertEqual(self.channel(name).get_playback_speed(), 1)
        self.assertEqual(self.channel(movie=True).get_playback_speed(), 1)
        with patch.object(renpy, "emscripten", True):
            self.assertEqual(self.channel().get_playback_speed(), 1)
            self.assertEqual(get_voice_speed(), 2)

    def test_context_restore_does_not_restore_preference(self):
        channel = self.channel()
        previous_context = channel.context.copy()
        set_voice_speed(2)
        renpy.game.context().music["voice"] = previous_context
        self.assertEqual(channel.get_playback_speed(), 2)

    def test_legacy_voice_channel_alias(self):
        channel = self.channel(2)
        with patch.dict(renpy.audio.audio.channels, {"voice": channel}):
            set_voice_speed(1.5)
            channel.enqueue(["a.ogg"], loop=False)
            set_voice_speed(3.0)
            channel.enqueue(["b.ogg"], loop=False)
            self.assertEqual([q.playback_speed for q in channel.queue], [1.5, 3.0])
            self.assertEqual(self.channel(0).get_playback_speed(), 1.0)
            self.assertEqual(self.channel("custom_voice").get_playback_speed(), 1.0)
            with patch.object(renpy, "emscripten", True):
                self.assertEqual(channel.get_playback_speed(), 1.0)


if __name__ == "__main__":
    unittest.main()
