# Copyright 2004-2026 Tom Rothamel <pytom@bishoujo.us>
#
# Permission is hereby granted, free of charge, to any person
# obtaining a copy of this software and associated documentation files
# (the "Software"), to deal in the Software without restriction,
# including without limitation the rights to use, copy, modify, merge,
# publish, distribute, sublicense, and/or sell copies of the Software,
# and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

"""
This file defines the API that Ren'Py uses to communicate with an audio and video
backend, and the default implementation of the API. This API is not intended
to be stable between multiple Ren'Py releases, and so is more intended for use
in ports to different platforms.

There are a few common variables with specific datatypes here.

`channel`
    An integer giving  the number of an audio channel.  These integers are
    allocated densely, but there's no limit to how many channels may be
    present at one time.

`file`
    This is an object representing an open file. This may be a Python file
    object, or a Ren'Py subfile object. All objects past to this have a
    `name` field, giving the name of the file. SubFiles also have `base`
    and length. Base is the offset from the start of the file where the data
    begins, while length is the amount of data in total.

Times and durations are represented as floats giving the number of seconds.

A channel may have up to two files associated with it, the playing file and
the queued file. The queued file begins playing when the current file ends
or is stopped.
"""

# When changing this API, change webaudio.py, too!

from __future__ import print_function

import renpy

from libc.stdint cimport uintptr_t, uint64_t
from libc.stdlib cimport malloc, free

import threading

from renpy.pygame.rwobject cimport RWopsFromPython
from sdl2 cimport SDL_RWops

cdef extern from "renpysound_core.h":

    ctypedef struct RPS_SpectrumSnapshot:
        uint64_t generation
        uint64_t count
        int sample_rate
        int fft_size
        int state
        double sample_time
        double position

    ctypedef struct RPS_SpectrumFFT:
        pass

    RPS_SpectrumFFT *RPS_spectrum_fft_new(int size)
    void RPS_spectrum_fft_free(RPS_SpectrumFFT *fft)
    void RPS_spectrum_fft_analyze(RPS_SpectrumFFT *fft, const float *pcm,
                                 double *magnitudes, double *rms, double *peak) noexcept nogil
    int RPS_set_spectrum_enabled(int channel, int size)
    int RPS_spectrum_device_available()
    int RPS_copy_spectrum(int channel, int size, uint64_t generation, uint64_t count,
                         int previous_state, double update_rate, float *pcm, RPS_SpectrumSnapshot *snapshot)
    int RPS_validate_spectrum(int channel, uint64_t generation, int state)

    void RPS_play(int channel, SDL_RWops *rw, char *ext, char* name, int synchro_start, int fadein, int tight, double start, double end, float volume, object audio_filter, double playback_speed, double video_playback_speed)
    void RPS_queue(int channel, SDL_RWops *rw, char *ext, char *name, int synchro_start, int fadein, int tight, double start, double end, float volume, object audio_filter, double playback_speed, double video_playback_speed)
    void RPS_seek(int channel, double position)
    int RPS_set_playback_speed(int channel, double speed)
    double RPS_get_playback_speed(int channel)
    void RPS_stop(int channel)
    void RPS_dequeue(int channel, int even_tight)
    int RPS_queue_depth(int channel)
    object RPS_playing_name(int channel)
    void RPS_fadeout(int channel, int ms)
    void RPS_pause(int channel, int pause)
    void RPS_unpause_all_at_start()
    void RPS_global_pause(int pause)
    int RPS_get_pos(int channel)
    double RPS_get_duration(int channel)
    void RPS_set_endevent(int channel, int event)
    void RPS_set_volume(int channel, float volume)
    float RPS_get_volume(int channel)
    void RPS_set_pan(int channel, float pan, float delay)
    void RPS_set_secondary_volume(int channel, float vol2, float delay)
    void RPS_replace_audio_filter(int channel, object audio_filter, int primary)

    void RPS_advance_time()
    int RPS_video_ready(int channel)
    object RPS_read_video(int channel)
    object RPS_read_video_yuv(int channel)
    int RPS_playing_at_end(int channel)
    void RPS_set_video(int channel, int video)

    void RPS_sample_surfaces(object, object)
    void RPS_init(int freq, int stereo, int samples, int status, int equal_mono, int linear_fades)
    void RPS_quit()

    void RPS_periodic()

    int RPS_get_sample_rate()
    char *RPS_get_error()
    void RPS_set_channel_count(int count)

    void (*RPS_generate_audio_c_function)(float *stream, int length)
    void (*RPS_apply_audio_filter)(object, float *stream, int length, int channels, int sample_rate)


from renpy.audio.filter cimport get_apply_audio_filter
RPS_apply_audio_filter = <void (*)(object, float *, int, int, int) noexcept> get_apply_audio_filter()


def check_error():
    """
    This is called by Ren'Py to check for an error. This function should raise
    a meaningful exception if an error has occurred in a background thread,
    or do nothing if an error has not occurred. (It should clear any error that
    it raises.)
    """

    e = RPS_get_error();
    if len(e):
        raise Exception(unicode(e, "utf-8", "replace"))


def play(channel, file, name, synchro_start=False, fadein=0, tight=False, start=0, end=0, relative_volume=1.0, audio_filter=None, playback_speed=1.0, video_playback_speed=None):
    """
    Plays `file` on `channel`. This clears the playing and queued samples and
    replaces them with this file.

    `name`
        A python object giving a readable name for the file.

    `paused`
        If True, playback is paused rather than started.

    `fadein`
        The time it should take the fade the music in, in seconds.

    `tight`
        If true, the file is played in tight mode. This means that fadeouts
        can span between this file and the file queued after it.

    `start`
        A time in the file to start playing.

    `end`
        A time in the file to end playing.    `

    `relative_volume`
        A float giving the relative volume of the file.

    `audio_filter`
        The audio filter to apply when the file is being played.
    """

    cdef SDL_RWops *rw

    playback_speed = renpy.preferences.validate_playback_speed(playback_speed)
    video_playback_speed = (renpy.preferences.validate_playback_speed(video_playback_speed)
                            if video_playback_speed is not None else 0.0)

    if audio_filter is not None:
        audio_filter.prepare(get_sample_rate())

    rw = RWopsFromPython(file)

    if rw == NULL:
        raise Exception("Could not create RWops.")

    if tight:
        tight = 1
    else:
        tight = 0

    name = name.encode("utf-8")
    RPS_play(channel, rw, name, name, synchro_start, fadein * 1000, tight, start, end, relative_volume, audio_filter, playback_speed, video_playback_speed)
    check_error()


def queue(channel, file, name, synchro_start=False, fadein=0, tight=False, start=0, end=0, relative_volume=1.0, audio_filter=None, playback_speed=1.0, video_playback_speed=None):
    """
    Queues `file` on `channel` to play when the current file ends. If no file is
    playing, plays it.

    The other arguments are as for play.
    """

    cdef SDL_RWops *rw

    playback_speed = renpy.preferences.validate_playback_speed(playback_speed)
    video_playback_speed = (renpy.preferences.validate_playback_speed(video_playback_speed)
                            if video_playback_speed is not None else 0.0)

    if audio_filter is not None:
        audio_filter.prepare(get_sample_rate())

    rw = RWopsFromPython(file)

    if rw == NULL:
        raise Exception("Could not create RWops.")

    if tight:
        tight = 1
    else:
        tight = 0

    name = name.encode("utf-8")
    RPS_queue(channel, rw, name, name, synchro_start, fadein * 1000, tight, start, end, relative_volume, audio_filter, playback_speed, video_playback_speed)
    check_error()


def stop(channel):
    """
    Immediately stops `channel`, and unqueues any queued audio file.
    """

    RPS_stop(channel)
    check_error()


def seek(channel, position):
    """
    Seeks the currently playing audio on `channel` to `position` seconds.

    This performs a lightweight seek inside the existing decode pipeline
    without tearing down and rebuilding the FFmpeg stream.  It is
    dramatically faster than calling play() with a ``<from X>`` clause —
    especially for encrypted audio files.

    `channel`
        The channel number to seek on.

    `position`
        The position in seconds to seek to.
    """

    RPS_seek(channel, position)
    check_error()


def set_playback_speed(channel, speed):
    speed = renpy.preferences.validate_playback_speed(speed)
    supported = RPS_set_playback_speed(channel, speed)
    check_error()
    if not supported:
        raise ValueError("Live playback speed changes require a movie with no audio track.")


def get_playback_speed(channel):
    speed = RPS_get_playback_speed(channel)
    check_error()
    return speed


def dequeue(channel, even_tight=False):
    """
    Dequeues the queued sound file.

    `even_tight`
        If true, a queued sound file that is tight is not dequeued. If false,
        a file marked as tight is dequeued.
    """

    RPS_dequeue(channel, even_tight)


def queue_depth(channel):
    """
    Returns the queue depth of the channel. 0 if no file is playing, 1 if
    a files is playing but there is no queued file, and 2 if a file is playing
    and one is queued.
    """

    return RPS_queue_depth(channel)


def playing_name(channel):
    """
    Returns the `name`  argument of the playing sound. This was passed into
    `play` or `queue`.
    """

    rv = RPS_playing_name(channel)

    if rv is not None:
        rv = rv.decode("utf-8")

    return rv


def pause(channel):
    """
    Pauses `channel`.
    """

    RPS_pause(channel, 1)
    check_error()


def unpause(channel):
    """
    Unpauses `channel`.
    """

    RPS_pause(channel, 0)
    check_error()


def global_pause(pause):
    """
    Pauses or unpauses all channels.
    """

    RPS_global_pause(pause)


def fadeout(channel, delay):
    """
    Fades out `channel` over `delay` seconds.
    """

    RPS_fadeout(channel, int(delay * 1000))
    check_error()


def busy(channel):
    """
    Returns true if `channel` is currently playing something, and false
    otherwise
    """

    return RPS_get_pos(channel) != -1


def get_pos(channel):
    """
    Returns the position of the audio file playing in `channel`, in seconds.
    Returns None if not file is is playing or it is not known.
    """

    return RPS_get_pos(channel) / 1000.0


def get_duration(channel):
    """
    Returns the duration of the audio file playing in `channel`, in seconds, or
    None if no file is playing or it is not known.
    """

    return RPS_get_duration(channel)


def set_volume(channel, volume):
    """
    Sets the primary volume for `channel` to `volume`, a number between
    0 and 1. This volume control is linear.
    """

    if volume == 0:
        RPS_set_volume(channel, 0)
    else:
        RPS_set_volume(channel, volume)

    check_error()


def set_pan(channel, pan, delay):
    """
    Sets the pan for channel.

    `pan`
        A number between -1 and 1 that control the placement of the audio.
        If this is -1, then all audio is sent to the left channel.
        If it's 0, then the two channels are equally balanced. If it's 1,
        then all audio is sent to the right ear.

    `delay`
        The amount of time it takes for the panning to occur.
    """

    RPS_set_pan(channel, pan, delay)
    check_error()


def set_secondary_volume(channel, volume, delay):
    """
    Sets the secondary volume for channel. This is linear, and is multiplied
    with the primary volume and scale factors derived from pan to find the
    actual multiplier used on the samples.

    `delay`
        The time it takes for the change in volume to happen.
    """

    RPS_set_secondary_volume(channel, volume, delay)
    check_error()


def replace_audio_filter(channel, audio_filter, playing):
    """
    Replaces the audio filter for `channel` with `audio_filter`.

    `playing`
        If true, the filter is applied to the currently playing file and queued file. If false,
        the filter is only applied to the queued file.
    """

    if audio_filter is not None:
        audio_filter.prepare(get_sample_rate())

    RPS_replace_audio_filter(channel, audio_filter, playing)


def deallocate_audio_filter(audio_filter):
    """
    Called when an audio filter is about to be dealloica to release all
    associated resources.
    """

    # Does nothing on this backend, but is used on web audio.


def get_volume(channel):
    """
    Gets the primary volume associated with `channel`.
    """

    return RPS_get_volume(channel)


def video_ready(channel):
    """
    Returns true if the video playing on `channel` has a frame ready for
    presentation.
    """

    return RPS_video_ready(channel)


def read_video(channel):
    """
    Returns the frame of video playing on `channel`. This should be returned
    as an SDL surface with 2px of padding on all sides.
    """

    rv = RPS_read_video(channel)

    if rv is None:
        return rv

    # Remove padding from the edges of the surface.
    w, h = rv.get_size()

    # This has to be set to the same number it is in ffmedia.c
    FRAME_PADDING = 4
    return rv.subsurface((FRAME_PADDING, FRAME_PADDING, w - FRAME_PADDING * 2, h - FRAME_PADDING * 2))


def read_video_yuv(channel):
    """Returns a capsule containing a packed YUV420 frame, or None."""

    return RPS_read_video_yuv(channel)


def playing_at_end(channel):
    """
    Returns true if the audio playing on `channel` has reached its natural
    end (playing_at_end flag). This means the stream has finished playback
    with nothing queued, but the stream is kept alive for seeking.
    """

    return bool(RPS_playing_at_end(channel))


# No video will be played from this channel.
NO_VIDEO = 0

# The video will be played while avoiding framedrops.
NODROP_VIDEO = 1

# The video will be played, allowing framedrops.
DROP_VIDEO = 2

def set_video(channel, video, loop=False):
    """
    Sets a flag that determines if this channel will attempt to decode video.

    `video`
        One of `NO_VIDEO`, `NODROP_VIDEO`, or `DROP_VIDEO`.

    `loop`
        If true, the video file will loop.
    """

    if video == NODROP_VIDEO:
        RPS_set_video(channel, NODROP_VIDEO)
    elif video:
        RPS_set_video(channel, DROP_VIDEO)
    else:
        RPS_set_video(channel, NO_VIDEO)


def init(freq, stereo, samples, status=False, equal_mono=False, linear_fades=False):
    """
    Initializes the audio system with the given parameters. The parameter are
    just informational - the audio system should be able to play all supported
    files.

    `freq`
        The sample frequency to play back at.

    `stereo`
        Should we play in stereo (generally true).

    `samples`
        The length of the sample buffer.

    `status`
        If true, the sound system will print errors out to the console.

    `equal_mono`
        If true, the sound system will play mono files at the same volume as
        stereo files.

    `linear_fades`
        If true, the sound system will use linear fades.
    """

    if status:
        status = 1
    else:
        status = 0

    RPS_init(freq, stereo, samples, status, equal_mono, linear_fades)
    check_error()


def quit(): # @ReservedAssignment
    """
    De-initializes the audio system.
    """

    RPS_quit()
    _spectrum_analyzers.clear()


def periodic():
    """
    Called periodically (at 20 Hz).
    """

    RPS_periodic()
    check_error()


def advance_time():
    """
    Called to advance time at the start of a frame.
    """

    RPS_advance_time()


def set_channel_count(count):
    """
    Sets the number of channels that the audio system should use
    """

    RPS_set_channel_count(count)


def get_sample_rate():
    """
    Returns the sample rate of the audio system.

    This is not part of the  api shared with other audio implementations
    """

    return RPS_get_sample_rate()


def set_generate_audio_c_function(fn):
    """
    This can be use to set a C function that totally replaces the Ren'Py
    audio system.

    The function is expected to have the signature void (*)(short *buf, int samples,
    and fill buf with samples samples of audio, where each sample consists of two
    shorts.

    This is not part of the  api shared with other audio implementations
    """

    global RPS_generate_audio_c_function

    if not isinstance(fn, int):
        import ctypes
        fn = ctypes.cast(fn, ctypes.c_void_p).value

    RPS_generate_audio_c_function = <void (*)(float *, int) noexcept> <uintptr_t> fn


# Store the sample surfaces so they stay alive.
rgb_surface = None
rgba_surface = None

def sample_surfaces(rgb, rgba):
    """
    Called to provide sample surfaces to the display system. The surfaces
    returned by read_video should be in the same format as these.
    """

    global rgb_surface
    global rgba_surface

    rgb_surface = rgb
    rgba_surface = rgb

    RPS_sample_surfaces(rgb, rgba)


# Is this the webaudio module?
is_webaudio = False


_spectrum_analyzers = {}
_spectrum_states = ("warming", "playing", "paused", "starved", "stopped")


cdef class _SpectrumAnalyzer:
    cdef int size
    cdef double rate
    cdef float *pcm
    cdef double *magnitudes
    cdef RPS_SpectrumFFT *fft
    cdef uint64_t generation, count, sequence
    cdef object cache, lock

    def __cinit__(self, int size, double rate):
        self.size = size
        self.rate = rate
        self.pcm = <float *> malloc(2 * size * sizeof(float))
        self.magnitudes = <double *> malloc((size // 2 + 1) * sizeof(double))
        self.fft = RPS_spectrum_fft_new(size)
        if self.pcm == NULL or self.magnitudes == NULL or self.fft == NULL:
            raise MemoryError("Unable to allocate spectrum analysis buffers.")
        self.lock = threading.Lock()

    def __dealloc__(self):
        free(self.pcm)
        free(self.magnitudes)
        RPS_spectrum_fft_free(self.fft)

    def read(self, int channel):
        cdef RPS_SpectrumSnapshot snapshot
        cdef int copied, previous_state
        cdef double rms, peak
        cdef uint64_t sequence
        # The query mutex protects only this workspace. The callback never
        # takes it. Keeping self alive also makes disable/quit safe during FFT.
        with self.lock:
            for attempt in range(3):
                previous_state = _spectrum_states.index(self.cache.state) if self.cache is not None else -1
                copied = RPS_copy_spectrum(channel, self.size, self.generation, self.count,
                                           previous_state, self.rate, self.pcm, &snapshot)
                if not copied:
                    return None
                state = _spectrum_states[snapshot.state]
                if copied == 1:
                    if self.cache.state != state:
                        self.cache = self.cache._replace(state=state)
                    return self.cache
                with nogil:
                    RPS_spectrum_fft_analyze(self.fft, self.pcm, self.magnitudes, &rms, &peak)
                # A seek, stream switch, close, or device rebuild while the GIL
                # was released invalidates the copied window. Never publish it.
                if not RPS_validate_spectrum(channel, snapshot.generation, snapshot.state):
                    continue
                sequence = self.sequence + 1 if self.generation == snapshot.generation else 1
                from renpy.audio.audio import SpectrumFrame
                frame = SpectrumFrame(
                    snapshot.generation, sequence, snapshot.sample_rate, self.size,
                    snapshot.sample_time if snapshot.sample_time >= 0 else None,
                    snapshot.position if snapshot.position >= 0 else None, state,
                    tuple(self.magnitudes[i] for i in range(self.size // 2 + 1)), rms, peak)
                self.generation = snapshot.generation
                self.count = snapshot.count
                self.sequence = sequence
                self.cache = frame
                return frame
            return None


def set_spectrum_enabled(int channel, enabled, fft_size=4096, update_rate=30.0):
    from renpy.audio.audio import validate_spectrum_config
    fft_size, update_rate = validate_spectrum_config(fft_size, update_rate)
    cdef _SpectrumAnalyzer old = _spectrum_analyzers.get(channel)
    cdef _SpectrumAnalyzer analyzer
    cdef int supported
    if channel < 0:
        raise ValueError("Audio channel number must be nonnegative.")
    if not enabled:
        supported = RPS_set_spectrum_enabled(channel, 0)
        _spectrum_analyzers.pop(channel, None)
    elif old is not None and old.size == fft_size and old.rate == update_rate:
        # Same configuration must still report device loss, without resetting
        # its ring or allocating another plan.
        return bool(RPS_spectrum_device_available())
    else:
        analyzer = _SpectrumAnalyzer(fft_size, update_rate)
        # Allocate before swapping the native ring. Failed allocation leaves
        # the old configuration usable. Rate changes also reset continuity.
        supported = RPS_set_spectrum_enabled(channel, fft_size)
        if supported > 0:
            _spectrum_analyzers[channel] = analyzer
    if supported < 0:
        raise MemoryError("Unable to configure spectrum capture.")
    return bool(supported)


def get_spectrum(int channel):
    if channel < 0:
        raise ValueError("Audio channel number must be nonnegative.")
    analyzer = _spectrum_analyzers.get(channel)
    if analyzer is None:
        return None
    return analyzer.read(channel)


def _spectrum_device_available():
    return bool(RPS_spectrum_device_available())

# When changing this API, change webaudio.py, too!
