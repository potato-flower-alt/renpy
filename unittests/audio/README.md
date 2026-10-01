`voice-tone.mp3` is a generated two-second, 440 Hz sine wave (mono, 44100 Hz).
It deliberately has no Xing header, so the decoder must handle unknown duration.
Generate a WAV with `AudioTempoTests.tone(channels=1)` and encode it with:

```sh
ffmpeg -i tone.wav -c:a libmp3lame -b:a 64k -write_xing 0 -id3v2_version 0 -map_metadata -1 voice-tone.mp3
```

Voice speed regression tests use the rebuilt engine and FFmpeg 7.1.1, including
`libavfilter` with `atempo` enabled. Run from the engine repository:

```sh
# Use the native SDK Python with the complete Python 3.12 unittest library.
python -m unittest unittests.test_voice_speed unittests.test_voice_runtime
python renpy.py testcases test voice_speed_controls
python renpy.py testcases test voice_speed_persistence

# Point PKG_CONFIG_PATH at the same native dependencies used for the engine.
cc -O1 -g -fsanitize=address,undefined unittests/audio_tempo_driver.c \
    -o /tmp/audio_tempo_driver \
    $(pkg-config --cflags --libs --static libavfilter libavformat libavcodec \
      libswresample libswscale sdl2) -lm -pthread
ASAN_OPTIONS=detect_leaks=1 RENPY_TEMPO_DRIVER=/tmp/audio_tempo_driver \
    python3 -m unittest unittests.test_audio_tempo
```

The offline driver consumes the actual decoder queue without a sound device.
It checks output sample counts, spectral energy and pitch, unchanged 1x PCM,
resampling, WAV/OGG/Opus/MP3, unknown duration, 5 ms clips, cropping, explicit
silence, malformed input, and repeated seeks after EOF. Mixer tests use SDL's
dummy audio driver for pause/resume, independent queued speeds, seeking during
playback, stopping, and fading. The preference tests include validation,
persistence, upgrades, queue capture, and the Web bypass. The Ren'Py testcases
exercise the real Actions/BarValue, completion callbacks, auto-forward delay,
save/load and rollback.

Empty reads and EOF must leave the tempo decoder asleep. The mixer tests also
queue new audio after natural completion and seek backwards from EOF, checking
that retaining an idle stream does not prevent playback from restarting.

Validation on 2026-09-24:

- All 8 decoder tests passed under AddressSanitizer/UndefinedBehaviorSanitizer,
  including leak detection (with all speed/format subcases).
- All 12 Python/native mixer tests passed on Windows x86_64 and Linux x86_64
  under WSL. SDL dummy playback verifies the mixer, not physical audio hardware.
- Both Ren'Py testcases passed. In this SDL2 branch, the test game's existing
  parallel image preloader can deadlock while quitting after load/rollback
  (`display/im.py`, image workers waiting on futures). The persistence test was
  also rerun successfully with `config.preload_threads = 1` in a temporary test
  configuration; no image-preloader changes are part of this feature.
- The GUI template was rendered at desktop and small/mobile variants; its
  reset button passed a click test. A simulated Web variant hides the controls.
- FFmpeg and the engine compiled and linked for Windows x86_64; Linux x86_64
  and aarch64; macOS x86_64 and arm64; and Android x86_64, arm64-v8a and
  armeabi-v7a. All eight native FFmpeg builds contain the atempo filter.
- Cython generation, new-test Ruff checks and `git diff --check` passed.
  The touched legacy Python modules still have pre-existing Ruff diagnostics
  for compatibility imports, re-exports and import placement.

iOS compilation, a complete Web build/browser run, physical-device listening,
and macOS/Android/Linux ARM execution were not tested in this environment.
The iOS link configuration and Web native-code guards have been updated.
The workspace Ren'Py SDK was subsequently updated at the user's request,
including the native libraries and the Live2D rendering fix described below.

Voiced-dialogue frame-time investigation (2026-09-24):

- A retained EOF stream was repeatedly waking its tempo decoder; empty reads
  now request no refill, and the mixer skips completed, unqueued channels.
- The sustained visible slowdown in the game also reproduced with the native
  library from before voice speed was implemented. The expensive frames were
  in GL drawing, with Live2D's full-canvas mask textures for a character and
  dialogue portrait. Cropping those textures to their geometry bounds restored
  approximately 60 FPS during playback, after EOF, and through transitions.
- The GL pixel test `live2d_cropped_masks` checks normal, inverted, combined,
  and empty masks, including samples outside the crop. It passed on Windows
  with the final SDK. Four fixed-pose image comparisons had mean per-channel
  differences below 0.004 on the 0-255 scale.
- Frame-time measurements used normal Windows/WASAPI playback, not the
  built-in test runner, which changes rendering behavior. The audio sample
  count/pitch and queue tests above remain separate from these GPU measurements.
