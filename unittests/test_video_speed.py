"""Public movie speed API tests; run with the SDK Python."""

from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import renpy

if not hasattr(renpy, "audio"):
    renpy.import_all()

from renpy.audio import audio, music
from renpy.display import video
from renpy.exports.mediaexports import movie_cutscene
from renpy.preferences import Preferences


class VideoSpeedTests(unittest.TestCase):
    def test_movie_shader_hook_is_bound_and_loadability_remains_static(self):
        movie = object.__new__(video.Movie)
        movie.mask = "mask.webm"
        movie.side_mask = False
        with patch.object(renpy.gl2.gl2shadercache, "predict_shader") as predict:
            self.assertEqual(movie.predict_shaders(("custom",)), [])
        self.assertEqual(predict.call_count, 3)
        predict.assert_any_call(("custom", "-renpy.texture", "renpy.movie_yuv"))
        self.assertIsInstance(video.Movie.__dict__["any_loadable"], staticmethod)

    def setUp(self):
        self.context = SimpleNamespace(music={}, init_phase=False)
        for p in (
            patch.object(renpy.game, "context", return_value=self.context),
            patch.object(renpy.game, "preferences", Preferences()),
            patch.object(renpy.game, "log", SimpleNamespace(mutated={})),
            patch.object(renpy.exports, "is_seen_allowed", return_value=False),
            patch.object(renpy.config, "debug_sound", True),
            patch.object(renpy.config, "skipping", None),
            patch.object(audio, "pcm_ok", False),
        ):
            p.start()
            self.addCleanup(p.stop)
        self.channel = audio.Channel("movie", False, False, False, "", "", False, True, True, False)
        p = patch.dict(audio.channels, {"movie": self.channel})
        p.start()
        self.addCleanup(p.stop)

    def test_play_queue_and_context(self):
        music.play("one.webm", channel="movie", loop=True, playback_speed=2)
        self.assertEqual(self.channel.queue[0].playback_speed, 2)
        saved = self.channel.context.copy()
        music.queue("two.webm", channel="movie", loop=True, clear_queue=False, playback_speed=0.5)
        self.assertEqual([q.playback_speed for q in self.channel.queue], [2, 0.5])
        self.assertEqual(self.channel.loop_playback_speed, 0.5)
        self.assertEqual(saved.last_playback_speed, 2)
        self.assertEqual(self.channel.context.last_playback_speed, 0.5)

    def test_loop_refill_without_queue_entry(self):
        music.play("one.webm", channel="movie", loop=True, playback_speed=2.5)
        self.channel.queue.clear()
        self.channel.actual_volume = 1.0
        self.channel.periodic()
        self.assertEqual(self.channel.queue[0].playback_speed, 2.5)

    def test_restore_same_filename_with_different_speed(self):
        for initial in (None, 1.5):
            with self.subTest(initial=initial):
                music.play("one.webm", channel="movie", loop=True, playback_speed=initial)
                saved = self.channel.context.copy()
                music.play("one.webm", channel="movie", loop=True, playback_speed=3)
                self.context.music["movie"] = saved
                with (
                    patch.object(audio, "pcm_ok", True),
                    patch.object(audio, "all_channels", [self.channel]),
                    patch.object(audio, "periodic"),
                    patch.object(self.channel, "interact"),
                    patch.object(self.channel, "fadeout") as fadeout,
                    patch.object(video, "last_channel_movie", {}),
                ):
                    audio.interact()
                fadeout.assert_called_once()
                self.assertEqual(self.channel.loop_playback_speed, initial or 1.0)

    def test_if_changed_updates_future_loops(self):
        music.play("one.webm", channel="movie", loop=True, playback_speed=1.5)
        with patch.object(self.channel, "get_playing", return_value="one.webm"):
            music.play("one.webm", channel="movie", loop=True, if_changed=True, playback_speed=3)
        self.assertEqual(self.channel.queue[0].playback_speed, 1.5)
        self.assertEqual(self.channel.loop_playback_speed, 3)

    def test_invalid_speed_fails_before_side_effects(self):
        for speed in (True, "2", 0, -1, float("nan"), float("inf"), 3.01, 10**1000):
            with self.subTest(speed=speed), patch.object(renpy.exports, "mode") as mode:
                for call in (
                    lambda: music.play("a.webm", channel="movie", playback_speed=speed),
                    lambda: music.queue("a.webm", channel="movie", playback_speed=speed),
                    lambda: movie_cutscene("a.webm", playback_speed=speed),
                    lambda: video.Movie(playback_speed=speed),
                    lambda: music.set_playback_speed(speed, channel="movie"),
                ):
                    with self.assertRaises((TypeError, ValueError)):
                        call()
                mode.assert_not_called()
        self.assertEqual(self.channel.queue, [])

    def test_mask_and_movie_receive_same_speed(self):
        movie = SimpleNamespace(
            mask="mask.webm", _play="color.webm", channel="movie", mask_channel="mask", loop=True, playback_speed=1.5
        )
        with patch.object(video, "find_oversampled", side_effect=lambda m, f: f), patch.object(music, "play") as play:
            video.default_play_callback(None, movie)
        self.assertEqual(len(play.call_args_list), 2)
        self.assertEqual([c.kwargs["playback_speed"] for c in play.call_args_list], [1.5, 1.5])

    def test_different_speed_restarts_same_movie(self):
        old = SimpleNamespace(_play="a.webm", playback_speed=1.0)
        new = SimpleNamespace(_play="a.webm", playback_speed=2.0, ensure_channels=Mock(), play_callback=Mock())
        with patch.object(renpy.config, "replay_movie_sprites", False):
            video.Movie.play(new, old)
            new.play_callback.assert_called_once_with(old, new)
            new.play_callback.reset_mock()
            old.playback_speed = 2.0
            video.Movie.play(new, old)
            new.play_callback.assert_not_called()

    def test_fullscreen_loops_forward_speed(self):
        with (
            patch.object(music, "play") as play,
            patch.object(video, "find_oversampled_filename", side_effect=lambda f: f),
        ):
            video.movie_start_fullscreen("a.webm", loops=2, playback_speed=3)
        self.assertEqual(play.call_args.args[0], ["a.webm"] * 3)
        self.assertEqual(play.call_args.kwargs["playback_speed"], 3)

    def test_old_context_and_movie_defaults(self):
        self.assertIsNone(audio.MusicContext().last_playback_speed)
        self.assertEqual(video.Movie.playback_speed, 1.0)

    def test_pending_request_stop_and_independent_play(self):
        self.assertEqual(music.get_playback_speed(channel="movie"), 1)
        music.set_playback_speed(0.5, channel="movie")
        self.assertEqual(music.get_playback_speed(channel="movie"), 1)
        music.play("one.webm", channel="movie", loop=True, playback_speed=2)
        music.set_playback_speed(0.5, channel="movie")
        self.assertEqual(music.get_playback_speed(channel="movie"), 0.5)
        self.assertEqual(self.channel.queue[0].playback_speed, 2)
        self.assertEqual(self.channel.context.last_playback_speed, 0.5)
        music.stop(channel="movie")
        self.assertEqual(music.get_playback_speed(channel="movie"), 1)
        music.play("two.webm", channel="movie", playback_speed=1.5)
        self.assertEqual(music.get_playback_speed(channel="movie"), 1.5)
        self.assertIsNone(self.channel.live_playback_speed)
        music.set_playback_speed(0.5, channel="movie")
        music.play("three.webm", channel="movie")
        self.assertEqual(music.get_playback_speed(channel="movie"), 1)

    def test_live_setter_does_not_replay_seek_or_dequeue(self):
        music.play("one.webm", channel="movie", loop=True)
        self.channel._number = 0
        self.channel.playback_speed_pending = False
        entries = self.channel.queue[:]
        with (
            patch.object(audio, "pcm_ok", True),
            patch.object(audio.renpysound, "get_playback_speed", return_value=1),
            patch.object(audio.renpysound, "set_playback_speed") as setter,
            patch.object(self.channel, "fadeout", side_effect=AssertionError("restart")),
            patch.object(self.channel, "enqueue", side_effect=AssertionError("enqueue")),
            patch.object(self.channel, "dequeue", side_effect=AssertionError("dequeue")),
            patch.object(self.channel, "seek", side_effect=AssertionError("seek")),
        ):
            music.set_playback_speed(0.5, channel="movie")
            music.set_playback_speed(0.5, channel="movie")
            setter.assert_called_once_with(0, 0.5)
            self.assertEqual(self.channel.queue, entries)
            self.assertEqual(music.get_playback_speed(channel="movie"), 0.5)

    def test_pending_and_subsequent_loops_install_rate_before_native_start(self):
        music.play("one.webm", channel="movie", loop=True, playback_speed=2)
        music.set_playback_speed(0.5, channel="movie")
        self.channel._number = 0
        self.channel.actual_volume = 1
        self.channel.buffer_queue = True
        with (
            patch.object(audio, "load", return_value=object()),
            patch.object(audio.renpysound, "set_video"),
            patch.object(audio.renpysound, "play") as play,
            patch.object(audio.renpysound, "queue") as queue,
            patch.object(audio.renpysound, "queue_depth", return_value=0) as depth,
        ):
            self.channel.periodic()
            self.assertEqual(play.call_args.kwargs["video_playback_speed"], 0.5)
            self.assertEqual(play.call_args.kwargs["playback_speed"], 2)
            self.assertFalse(self.channel.playback_speed_pending)
            depth.return_value = 1
            self.channel.periodic()
            self.assertEqual(queue.call_args.kwargs["video_playback_speed"], 0.5)
            self.assertEqual(queue.call_args.kwargs["playback_speed"], 2)

    def test_other_channels_and_backend_rejection(self):
        music.play("one.webm", channel="movie", loop=True)
        self.channel._number = 0
        self.channel.playback_speed_pending = False
        with (
            patch.object(audio, "pcm_ok", True),
            patch.object(audio.renpysound, "get_playback_speed", return_value=1),
            patch.object(audio.renpysound, "set_playback_speed", side_effect=ValueError("audio track")),
        ):
            with self.assertRaises(ValueError):
                music.set_playback_speed(0.5, channel="movie")
        self.assertIsNone(self.channel.live_playback_speed)
        self.assertEqual(self.channel.requested_playback_speed, 1)
        self.channel.movie = audio.renpysound.NO_VIDEO
        with self.assertRaises(ValueError):
            music.set_playback_speed(0.5, channel="movie")

    def test_side_mask_retains_texture_and_uses_one_frame_for_both_halves(self):
        music.play("one.webm", channel="movie", loop=True)
        previous, replacement = object(), object()
        frame, color, mask = Mock(), Mock(), Mock()
        frame.get_size.return_value = (64, 64)
        frame.subsurface.side_effect = [mask, color]
        with (
            patch.object(video, "texture", {"movie": previous}),
            patch.object(music, "get_playing", return_value="one.webm"),
            patch.object(self.channel, "read_video", side_effect=[None, frame]) as read,
            patch.object(renpy.display.module, "alpha_munge") as alpha,
            patch.object(renpy.display.render, "mutated_surface"),
            patch.object(renpy.display, "draw", SimpleNamespace(load_texture=Mock(return_value=replacement))),
        ):
            music.set_playback_speed(0.5, channel="movie")
            read.assert_not_called()
            self.assertEqual(video.get_movie_texture("movie", side_mask=True, mipmap=False), (previous, False))
            self.assertEqual(video.get_movie_texture("movie", side_mask=True, mipmap=False), (replacement, True))
            alpha.assert_called_once_with(mask, color, renpy.display.im.identity)
            self.assertEqual([c.args[0] for c in frame.subsurface.call_args_list], [(32, 0, 32, 64), (0, 0, 32, 64)])

    def test_web_live_api_is_explicitly_unsupported(self):
        with patch.object(renpy, "emscripten", True):
            with self.assertRaises(NotImplementedError):
                music.set_playback_speed(0.5, channel="movie")
            with self.assertRaises(NotImplementedError):
                music.get_playback_speed(channel="movie")


if __name__ == "__main__":
    unittest.main()
