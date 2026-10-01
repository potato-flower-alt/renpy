init python:
    def _test_voice_speed_controls():
        original = renpy.get_voice_speed()
        try:
            action = Preference("voice speed", 1.5)
            renpy.run(action)
            assert renpy.get_voice_speed() == 1.5
            assert action.get_selected()

            value = Preference("voice speed")
            adjustment = value.get_adjustment()
            assert adjustment.range == 2.5
            assert adjustment.value == 1.0
            assert adjustment.step == 0.05
            value.changed(2.5)
            assert renpy.get_voice_speed() == 3.0

            renpy.run(Preference("voice speed", 1.0))
            assert renpy.get_voice_speed() == 1.0
            assert not action.get_selected()
        finally:
            renpy.set_voice_speed(original)

    def _test_voice_speed_completion():
        # This branch retains the native stream at EOF to support backwards
        # seeking. Completion must use playing_at_end, not playing_name alone.
        channel = renpy.audio.audio.get_channel("voice")
        original_playing = renpy.sound.is_playing
        original_end = channel.playing_at_end
        original_callbacks = config.voice_callbacks
        original_info = _voice.playing_info
        original_last = _voice.last_playing
        original_wait = preferences.wait_voice
        events = []
        ended = [False]
        periodic = next(i for i in config.periodic_callbacks if i.__name__ == "_voice_periodic_callback")
        try:
            renpy.sound.is_playing = lambda **kwargs: True
            channel.playing_at_end = lambda: ended[0]
            config.voice_callbacks = [lambda event, info, **kwargs: events.append(event)]
            preferences.wait_voice = True
            _voice.playing_info = object()
            assert not config.afm_callback()
            periodic()
            assert not events

            ended[0] = True
            periodic()
            periodic()
            assert events == ["stop"]
            assert _voice.playing_info is None
            assert not config.afm_callback()
            _voice.last_playing -= config.afm_voice_delay + 1
            assert config.afm_callback()
        finally:
            renpy.sound.is_playing = original_playing
            channel.playing_at_end = original_end
            config.voice_callbacks = original_callbacks
            _voice.playing_info = original_info
            _voice.last_playing = original_last
            preferences.wait_voice = original_wait

testcase voice_speed_controls:
    $ _test_voice_speed_controls()
    $ _test_voice_speed_completion()

default _voice_speed_save_marker = 1

label _voice_speed_save_test:
    "Voice speed save point."
    "Voice speed later line."
    return

testsuite voice_speed_persistence:
    setup:
        $ _test.voice_speed_state = (renpy.get_voice_speed(), "voice-speed-test-" + str(renpy.time.time()))

    teardown:
        $ renpy.unlink_save(_test.voice_speed_state[1])
        $ renpy.set_voice_speed(_test.voice_speed_state[0])
        $ del _test.voice_speed_state

    testcase save_and_rollback:
        $ renpy.set_voice_speed(1.0)
        run Start("_voice_speed_save_test")
        assert "Voice speed save point."
        $ renpy.save(_test.voice_speed_state[1])
        $ renpy.set_voice_speed(2.5)
        advance until "Voice speed later line."
        run Rollback()
        assert "Voice speed save point."
        assert eval renpy.get_voice_speed() == 2.5
        $ renpy.store._voice_speed_save_marker = 2
        # Loading transfers control. Schedule it after this test node returns.
        $ renpy.invoke_in_main_thread(renpy.load, _test.voice_speed_state[1])
        pause 0.2
        assert "Voice speed save point."
        assert eval renpy.store._voice_speed_save_marker == 1
        assert eval renpy.get_voice_speed() == 2.5
        run MainMenu(confirm=False)
