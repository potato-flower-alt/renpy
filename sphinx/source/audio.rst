.. _audio:

Audio
=====

Ren'Py supports playing music and sound effects in the background,
using the following audio file formats:

* Opus
* Ogg Vorbis
* MP3
* MP2
* FLAC
* WAV (uncompressed 16-bit or 24-bit signed PCM)

On the web browser, Ren'Py will check a list of audio formats, and
enable a mode that is faster and less prone to skipping if the web
browser supports all modes on the list. If your game is using only
mp3s, and skips on Safari, then consider changing :var:`config.webaudio_required_types`.

Ren'Py supports an arbitrary number of audio channels. There are two
normal channels defined by default:

* ``music`` - A channel for music playback.
* ``sound`` - A channel for sound effects.

Normal channels support playing and queueing audio, but only play back
one audio file at a time. New normal channels can be registered with
:func:`renpy.music.register_channel`.

The Music Volume, Sound Volume, and Voice Volume settings
of the in-game preferences menu are used to set individual
volumes for these channels. See :ref:`volume` for more information.

In addition to the normal channels, there are two special channels:

* ``audio`` - The audio channel supports playing back multiple audio files at one time,
   but does not support queueing sound or stopping playback.
* ``voice`` - The voice chanel is used by Ren'Py's voice system. Ren'Py will automaticall
  play and stop voice files as characters speak, and may adjust the volume of the voice channel.

Sounds can also be set to play when buttons, menu choices, or
imagemaps enter their hovered or activated states. See
:ref:`Button Style Properties <button-style-properties>`. Two configuration
variables, :var:`config.main_menu_music` and :var:`config.game_menu_music` allow
for the given music files to be played as the main and game menu music,
respectively.

In-game, the usual way to play music and sound in Ren'Py is using
the three music/sound statements. Audio files can either be provided directly
as strings, or as defined names within the audio namespace.

.. _audio-namespace:

Audio Directory and Namespace
-----------------------------

When Ren'Py searches for an audio file used by audio statements or functions, it
will start inside the ``game`` directory. If the provided file is not found there,
it will also look in the ``game/audio`` directory. For example::

    play music "opening.ogg"

will first look for :file:`game/opening.ogg`. If not found, Ren'Py will look for
:file:`game/audio/opening.ogg`. This is consistent with audio files in further
subdirectories. The following statement::

    play music "my_music/opening.ogg"

will first look for :file:`game/my_music/opening.ogg`, before looking for
:file:`game/audio/my_music/opening.ogg` if the former is not found.

Files found in the ``game/audio`` directory, **as well as its subdirectories**, that end with
a supported extension (currently, .wav, .mp2, .mp3, .ogg, and .opus) are also automatically
placed by Ren'Py into the audio namespace. This means that :file:`game/audio/Town_theme.ogg`
can be played with::

    play music town_theme

The directories that are searched for files can be changed by modifying :var:`config.audio_directories`.

The usable name is determined by stripping the extension and forcing the rest of
the filename to lower case. This is only the case for audio files with names that
can be expressed as Python variables, for example: :file:`my song.mp3`, :file:`8track.opus`,
or :file:`this-is-a-song.ogg` will not work. Additionally, if two or more files would
end up defined under the same name, only the first file (determined alphabetically by its path
and filename, extension included) will be defined.

When a file isn't placed automatically into the audio namespace, either due to
an incompatible name or being inside a different directory, it can still be placed
there manually with the define statement.

For example, one can write::

    define audio.sunflower = "my_music/sun-flower-slow-jam.ogg"

and then use::

    play music sunflower

The ``play`` and ``queue`` statements always evaluate their arguments in the
audio namespace. Functions do not, meaning they will not work with ``sunflower``,
but will work with ``audio.sunflower`` instead.

.. _play-statement:

Play Statement
--------------

The ``play`` statement is the most common way used to play sound and music.
If a file is currently playing on a normal channel, it is interrupted and
replaced with the new file.

The name of a channel is expected following the keyword ``play``.
(Usually, this is either "sound", "music", "voice", or "audio"). This is
followed by audiofile(s), where audiofile(s) can be one file or list of files.
When the list is given, the item of it is played in order.

The ``fadein`` and ``fadeout`` clauses are optional. Fadeout gives the fadeout
time for currently playing music, in seconds, while fadein gives the time
it takes to fade in the new music. If fadeout is not given, :var:`config.fadeout_audio`
is used.

The ``loop`` and ``noloop`` clauses are also optional. The loop clause causes
the music to loop, while noloop causes it to play only once. If neither of them are
given, the default of the channel is used. ::

        play music "mozart.ogg"
        play sound "woof.mp3"
        play myChannel "punch.wav" # 'myChannel' needs to be defined with renpy.music.register_channel().

        "We can also play a list of sounds, or music."
        play music [ "a.ogg", "b.ogg" ] fadeout 1.0 fadein 1.0

When the ``if_changed`` clause is provided, and if the given track is currently playing
on the channel, the play instruction doesn't interrupt it. ::

        label market_side:
            play music market
            "We're entering the market."
            jump market_main

        label market_main:
            play music market if_changed
            "Maybe we just entered the market, maybe we were already there."
            "If we were already there, the music didn't stop and start over, it just continued."
            jump market_main

The ``volume`` clause is also optional, and specifies a relative amplitude for
the track, between 0.0 and 1.0. This makes it possible to adjust the amplitude a
track is played at, each time it's played. ::

        play sound "woof.mp3" volume 0.5

On the audio channel, multiple play statements play multiple sounds at the same
time::

        play audio "sfx1.opus"
        play audio "sfx2.opus"

A variable may be used instead of a string here. If a variable exists in the
:ref:`audio namespace <audio-namespace>`, it's used in preference to the default namespace::

        play music illurock

Files placed into the audio namespace may automatically define variables that can
be used like this.

.. _synchro-start:

Ren'Py supports a feature that can ensure that audio files start playing at the same time. This feature
is enabled on looping audio channels (like music) by default, but can also be enabled by the
`synchro_start` option to :func:`renpy.music.register_channel` or :func:`renpy.music.play`.

When synchro start is enabled and multiple play statements are run at the same time, the audio in each channel
will start synchronized. Specifically, the audio will start:

* When the audio files on every channel have been loaded and audio samples are available.
* When all all channels have been faded out.

New audio will start playing when both conditions are met.


Stop Statement
--------------

The ``stop`` statement begins with the keyword ``stop``, followed by the name of a
channel to stop sound on. It may optionally have a ``fadeout`` clause. If the
fadeout clause is not given, :var:`config.fadeout_audio` is used. ::

        stop sound
        stop music fadeout 1.0


Queue Statement
---------------

The ``queue`` statement is used to queue up audio files. They will be played when
the channel finishes playing the currently playing file.

The queue statement begins with keyword ``queue``, followed by the name of a
channel to play sound on. It optionally takes the ``fadein``, ``loop`` and ``noloop`` clauses. ::

        queue sound "woof.mp3"
        queue music [ "a.ogg", "b.ogg" ]

Queue also takes the ``volume`` clause. ::

        play sound "woof.mp3" volume 0.25
        queue sound "woof.mp3" volume 0.5
        queue sound "woof.mp3" volume 0.75
        queue sound "woof.mp3" volume 1.0

When multiple queue statements are given without an interaction between them,
all sound files are added to the queue. After an interaction has occurred, the
first queue statement clears the queue, unless it has already been cleared by
a play or stop statement.

A variable may be used instead of a string here. If a variable exists in the
:ref:`audio namespace <audio-namespace>`, it's used in preference to the default namespace::

    define audio.woof = "woof.mp3"

    # ...

    play sound woof

The advantage of using these statements is that your program will be checked for
missing sound and music files when lint is run. The functions below exist to allow
access to allow music and sound to be controlled from Python, and to expose
advanced (rarely used) features.


.. _partial-playback:

Partial Playback
----------------

Ren'Py supports partial playback of audio files. This is done by putting a playback
specification, enclosed in angle brackets, at the start of the file.
The partial playback specification should consist of alternating
property name and value pairs, with every thing separated by spaces.

The values are always interpreted as seconds from the start of the file.
The three properties are:

``from``
    Specifies the position in the file at which the first play-through
    begins playing. (This defaults to 0.0 seconds.)

``to``
    Specifies the position in the file at which the file ends playing.
    (This defaults to the full duration of the file.)

``loop``
    Specifies the position in the file at which the second and later
    play-throughs begin playing. (This defaults to the start time
    given by ``from`` if specified, or to the start of the file.)

For example::

        play music "<from 5 to 15.5>waves.opus"

will play 10.5 seconds of waves.opus, starting at the 5 second mark. The statement::

        play music "<loop 6.333>song.opus"

will play song.opus all the way through once, then loop back to the 6.333
second mark before playing it again all the way through to the end.


.. _sync-start:

Sync Start Position
-------------------

The position in the file at which the clip begins playing can also be synced to
another channel with a currently-playing track using a filename like
"<sync channelname>track.opus", where channelname is the name of the channel,
which could be "music", "sound", or any other registered channels.

This can be used to sync multi-layered looping tracks together. For example::

        play music_2 [ "<sync music_1>layer_2.opus", "layer_2.opus" ]

Will play :file:`layer_2.opus` with the start time synced to the current track in
channel music_1 in the first iteration, before playing the whole track in
subsequent iterations. (By default, the :file:`layer_2.opus` start time will remain
modified even in subsequent iterations in the loop.)


.. _volume:

Volume
------

The volume at which a given track is going to be played depends on a number
of variables:

- the "main" mixer's volume
- the volume of the mixer which the channel relates to
- the volume of the channel
- the relative amplitude of the track itself
- the relative amplitude associated with the filename

These four volumes are values between 0 and 1, and their multiplication results
in the volume the track will be played at.

For example, if the main volume is 80% (or 0.8), the mixer's volume is 100%,
the channel volume is 50% (0.5) and the track's relative volume is 25% (0.25),
and the filename's relative volume is 50% (0.5), the resulting volume is .8\*1.\*.5\*.25\*.5 = .0.5 so 5%.

Note that while all of these volumes are amplitudes, the mixers are presented as
decibels in the preferences menu.

The mixers' volumes can be set using :func:`preferences.set_mixer`, using the
:func:`SetMixer` action, or using the :func:`Preference` action with the
``"mixer <mixer> volume"`` key.
The "audio" and "sound" channels relate to the "sfx" mixer, the "music" channel
to the "music" mixer and the "voice" channel to the "voice" mixer.
Every channel additionally relates to the "main" mixer, as shown above.

A channel's volume can be set using :func:`renpy.music.set_volume`. It is only
useful when several channels use the same mixer. The ``mixer`` parameter of the
:func:`renpy.music.register_channel` function sets to which mixer the registered
channel relates, creating it in the process if it doesn't already exist.

A track's relative volume is set with the ``volume`` clause of the :ref:`play-statement`.

A filename's relative volume is set with a volume clause in angle brackets,
such as "<volume 0.5>track.opus".

In addition to these volume values, there is the mute flag of the mixer which
the channel relates to. If enabled, it will reduce the played volume to 0.
They can be set using the :func:`SetMute` or :func:`ToggleMute` actions, or
using the :func:`Preference` action with the "mixer <mixer> mute" key, or using
the :func:`preferences.set_mute` function.


.. _silence:

Playing Silence
---------------

A specified duration of silence can played using a filename like
"<silence 3.0>", where 3.0 is the number of seconds of silence that is
desired. This can be used to delay the start of a sound file. For example::

        play audio [ "<silence .5>", "boom.opus" ]

Will play silence for half a second, and then an explosion sound.


.. _live-spectrum:

Live Spectrum
-------------

``renpy.music.set_spectrum_enabled(True, channel="music", fft_size=4096,
update_rate=30.0)`` enables live analysis of the named channel. Disable it with
``renpy.music.set_spectrum_enabled(False, channel="music")`` when the last
consumer exits. This is a shared switch, not a reference count. Native backends
support capture; browser audio returns False and ``get_spectrum`` returns None.
Invalid parameters raise ValueError even when capture is unsupported.

``renpy.music.get_spectrum(channel="music")`` returns an immutable
``renpy.audio.audio.SpectrumFrame`` with these attributes:

* ``generation`` marks a discontinuity: enable/configuration or device rebuild,
  new playback, an actual queue/loop stream switch, seek, or clearing on stop.
  Reset visual smoothing when it changes. Capture settings are process-local;
  saves and rollback do not restore them, while restored playback resets history.
* ``sequence`` increases for each newly published analysis window within a
  generation. Repeated queries of the same window preserve it and reuse the
  cached snapshot. A state-only change also preserves the sequence.
* ``sample_rate`` is the output PCM rate; ``fft_size`` is N.
* ``sample_time`` is the window's final output frame time in seconds within the
  current device session, not the query time. It is None before sampling.
* ``position`` is the source-file position at that window's end, in seconds,
  including filename start offsets and playback-speed mapping. It is None
  when unavailable. Decoder starvation does not advance this source position.
* ``state`` is ``warming`` while fewer than N frames have been collected,
  ``playing`` during normal output, ``paused`` while frozen, ``starved`` when
  missing decoder output is being filled with silence, or ``stopped`` after
  explicit stop or natural completion. Synchro-start waits in ``warming``.
* ``magnitudes`` is a tuple of N/2+1 finite, linear amplitudes. Bin k represents
  k times sample_rate/N Hz, including DC and Nyquist.
* ``rms`` is the root mean square of both channels in the unwindowed PCM window;
  ``peak`` is the maximum absolute sample in either channel in that window.

Analysis measures this channel's contribution after filters, stereo pan, fades,
relative volume and mixer volume, before final mono downmix and clipping. Other
channels, device/system volume, and clipping of the final mix are excluded.
Values are not clamped to [0, 1]; filters or gain can amplify the contribution.
Muted playback continues collecting zero contributions; ``stop_on_mute`` clears
the history. Pausing freezes the published window and sequence; resuming keeps
the ring. A stopped snapshot contains zeros and no sample time or position.
Disabled capture returns None and performs no FFT.
If playback changes repeatedly while a query is computing, that query may
return None; retrying reads a fresh window rather than publishing stale data.

Each stereo channel is transformed separately with a periodic Hann window.
The single-sided amplitudes are corrected by its coherent gain (sum of window
weights), with a factor of two except at DC and Nyquist. The channels' squared
FFT magnitudes are averaged before taking the square root, so opposite-phase
stereo does not cancel. For a bin-aligned stereo sine of amplitude A at the
capture point, its main bin is approximately A and its rms approximately
A/sqrt(2). This is an amplitude spectrum, not a power spectral density; use
``rms`` rather than summing bins to measure overall level.

Early windows are left-padded with zeros. Analysis is limited by output sample
counts at the configured update rate; a new generation or state is reported
immediately. Capture occurs in the mixer, with FFT performed during queries
outside the audio lock. Games supply their own log-frequency bands, level
compression, smoothing, and visual mapping. At 48 kHz, 4096 frames span about
85.3 ms and resolve about 11.72 Hz per bin. Expect about half a window of visual
response delay plus the audio device's output buffering.


Actions
-------

See :ref:`audio-actions`.


Functions
---------

.. include:: inc/audio


Sound Functions
---------------

Most ``renpy.music`` functions have aliases in ``renpy.sound``. These functions are similar,
except they default to the sound channel rather than the music channel, and default
to not looping.
