/* Deterministic tests of the actual mixer, with a fake decoder/device.
 * Compile with function sections and --gc-sections (see the build report).
 * No Python interpreter, threads, or real audio device is required. */
#include "../src/renpysound_core.c"
#include <assert.h>
#include <limits.h>

struct MediaState {
    int frame, remaining, finished, seeking;
    double start;
};

void SDL_LockAudio(void) {}
void SDL_UnlockAudio(void) {}
void SDL_PauseAudio(int pause) {}
int SDL_PushEvent(SDL_Event *event) { return 0; }
int SDL_LockMutex(SDL_mutex *mutex) { return 0; }
int SDL_UnlockMutex(SDL_mutex *mutex) { return 0; }
PyObject _Py_NoneStruct;
static SDL_AudioStatus device_status = SDL_AUDIO_PLAYING;
SDL_AudioStatus SDL_GetAudioStatus(void) { return device_status; }

int media_read_audio(MediaState *m, Uint8 *stream, int bytes) {
    if (m->seeking) return 0;
    int frames = min(bytes / 4, m->remaining);
    short *pcm = (short *) stream;
    for (int i = 0; i < frames; i++) {
        short v = (short) (12000 * sin(2 * PI * 16 * m->frame / 1024));
        pcm[2 * i] = v;
        pcm[2 * i + 1] = -v;
        m->frame++;
    }
    m->remaining -= frames;
    return 4 * frames;
}

int media_audio_finished(MediaState *m) { return m->finished; }
double media_audio_position(MediaState *m) { return m->start + (double) m->frame / audio_spec.freq; }
double media_video_position(MediaState *m) { return -1; }
void media_pause(MediaState *m, int pause) {}
double media_seek(MediaState *m, double target) {
    m->start = target;
    m->frame = 0;
    m->seeking = 1;
    m->finished = 0;
    return target;
}

static void filter(PyObject *unused, float *pcm, int channels, int frames, int rate) {
    for (int i = 0; i < channels * frames; i++) pcm[i] *= 3;
}

static void setup(MediaState *m) {
    struct Spectrum *s = channels[0].spectrum;
    if (s) { free(s->pcm); free(s); }
    memset(&channels[0], 0, sizeof(channels[0]));
    memset(m, 0, sizeof(*m));
    m->remaining = INT_MAX;
    struct Channel *c = &channels[0];
    c->playing = m;
    c->playing_speed = 1;
    c->playing_relative_volume = 0.8;
    c->mixer_volume = 0.7;
    init_interpolate(&c->pan, 0.3);
    init_interpolate(&c->fade, MAX_POWER);
    init_interpolate(&c->secondary_volume, MAX_POWER);
    c->stop_samples = -1;
    c->last_playing = 1;
    c->last_volume = 0.4;
    output_samples = 0;
}

int main(void) {
    initialized = 1;
    audio_spec.freq = 48000;
    assert(!check_channel(0));
    MediaState m;
    float off[2048], on[2048], pcm[8192];
    RPS_SpectrumSnapshot snapshot;
    /* Capture does not change any output bits, including filters, pan,
     * gain smoothing and clipping. */
    for (int mono = 1; mono <= 2; mono++) {
        channel_count = mono;
        setup(&m);
        RPS_apply_audio_filter = filter;
        channels[0].playing_audio_filter = (PyObject *) &m;
        callback(NULL, (Uint8 *) off, sizeof(off));
        setup(&m);
        channels[0].playing_audio_filter = (PyObject *) &m;
        assert(RPS_set_spectrum_enabled(0, 1024) == 1);
        callback(NULL, (Uint8 *) on, sizeof(on));
        assert(!memcmp(off, on, sizeof(on)));
    }
    channels[0].playing_audio_filter = NULL;
    RPS_apply_audio_filter = NULL;
    channel_count = 2;
    assert(RPS_copy_spectrum(0, 1024, 0, 0, -1, 30, pcm, &snapshot) == 2);
    assert(snapshot.count == 1024 && snapshot.state == RPS_SPECTRUM_PLAYING);
    assert(fabs(snapshot.position - 1024.0 / 48000) < 1e-9);
    assert(fabs(snapshot.sample_time - 1024.0 / 48000) < 1e-9);
    uint64_t generation = snapshot.generation;
    RPS_pause(0, 1);
    callback(NULL, (Uint8 *) on, sizeof(on));
    RPS_copy_spectrum(0, 1024, generation, 1024, RPS_SPECTRUM_PLAYING, 30, pcm, &snapshot);
    assert(snapshot.state == RPS_SPECTRUM_PAUSED && snapshot.count == 1024);
    RPS_pause(0, 0);
    /* Starvation writes actual silence and holds the source position. */
    m.remaining = 0;
    callback(NULL, (Uint8 *) on, sizeof(on));
    RPS_copy_spectrum(0, 1024, 0, 0, -1, 30, pcm, &snapshot);
    assert(snapshot.state == RPS_SPECTRUM_STARVED && snapshot.count == 2048);
    assert(fabs(snapshot.position - 1024.0 / 48000) < 1e-9);
    for (int i = 0; i < 2048; i++) assert(pcm[i] == 0);
    /* Seek must discard old PCM throughout the pending seek interval. */
    RPS_seek(0, 0.5);
    callback(NULL, (Uint8 *) on, sizeof(on));
    RPS_copy_spectrum(0, 1024, 0, 0, -1, 30, pcm, &snapshot);
    assert(snapshot.generation != generation && snapshot.state == RPS_SPECTRUM_STARVED);
    for (int i = 0; i < 2048; i++) assert(pcm[i] == 0);
    m.seeking = 0;
    m.remaining = INT_MAX;
    callback(NULL, (Uint8 *) on, sizeof(on));
    RPS_copy_spectrum(0, 1024, 0, 0, -1, 30, pcm, &snapshot);
    assert(snapshot.position > 0.5);
    channels[0].playing_speed = 2;
    m.start = NAN;
    callback(NULL, (Uint8 *) on, sizeof(on));
    RPS_copy_spectrum(0, 1024, 0, 0, -1, 30, pcm, &snapshot);
    assert(snapshot.position < 0);
    /* Natural EOF clears energy despite the retained MediaState. */
    m.remaining = 0;
    m.finished = 1;
    callback(NULL, (Uint8 *) on, sizeof(on));
    RPS_copy_spectrum(0, 1024, 0, 0, -1, 30, pcm, &snapshot);
    assert(snapshot.state == RPS_SPECTRUM_STOPPED && !snapshot.count);
    assert(snapshot.sample_time < 0 && snapshot.position < 0);
    for (int i = 0; i < 2048; i++) assert(pcm[i] == 0);
    /* Warming is left-padded, and a queue switch keeps only the new stream. */
    setup(&m);
    RPS_set_spectrum_enabled(0, 1024);
    callback(NULL, (Uint8 *) on, 128 * 2 * sizeof(float));
    RPS_copy_spectrum(0, 1024, 0, 0, -1, 30, pcm, &snapshot);
    assert(snapshot.state == RPS_SPECTRUM_WARMING && snapshot.count == 128);
    for (int i = 0; i < (1024 - 128) * 2; i++) assert(pcm[i] == 0);
    generation = snapshot.generation;
    MediaState next = {0, INT_MAX, 0, 0, 2};
    m.remaining = 0;
    m.finished = 1;
    channels[0].playing_name = strdup("old");
    channels[0].queued = &next;
    channels[0].queued_name = strdup("next");
    channels[0].queued_start_ms = 2000;
    channels[0].queued_relative_volume = 1;
    channels[0].queued_speed = 1;
    callback(NULL, (Uint8 *) on, sizeof(on));
    RPS_copy_spectrum(0, 1024, 0, 0, -1, 30, pcm, &snapshot);
    assert(snapshot.generation != generation && snapshot.count == 1024);
    assert(fabs(snapshot.position - (2 + 1024.0 / 48000)) < 1e-9);
    assert(RPS_validate_spectrum(0, snapshot.generation, snapshot.state));
    assert(!RPS_validate_spectrum(0, generation, snapshot.state));
    generation = snapshot.generation;
    device_status = SDL_AUDIO_STOPPED;
    assert(!RPS_copy_spectrum(0, 1024, 0, 0, -1, 30, pcm, &snapshot));
    assert(!RPS_spectrum_device_available());
    assert(!RPS_set_spectrum_enabled(0, 1024));
    assert(channels[0].spectrum->generation != generation);
    assert(!channels[0].spectrum->count);
    assert(!RPS_set_spectrum_enabled(0, 0));
    assert(!channels[0].spectrum);
    device_status = SDL_AUDIO_PLAYING;
    int count = num_channels;
    assert(!RPS_copy_spectrum(999, 1024, 0, 0, -1, 30, pcm, &snapshot));
    assert(num_channels == count);
    RPS_set_spectrum_enabled(0, 0);
    free(channels[0].playing_name);
    free(dying);
    free(channels);
    puts("Mixer: bit-identical output, filters, mono, clocks, pause, starvation, seek, EOF, warming, queue, device loss, no query allocation PASS");
    return 0;
}
