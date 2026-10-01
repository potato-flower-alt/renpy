/* Real decoder, deterministic monotonic clock. See test_video_decoder.py. */
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
static _Atomic int64_t ticks = 100000000;
#define av_gettime_relative test_clock_now
#include "../src/ffmedia.c"
int64_t test_clock_now(void) { return atomic_load(&ticks); }
static void advance(double seconds) { atomic_fetch_add(&ticks, llround(seconds * 1000000)); }
static void near(double a, double b) { assert(fabs(a - b) < 0.000001); }

static MediaState *open_movie(const char *filename, int dropping) {
    MediaState *ms = media_open(SDL_RWFromFile(filename, "rb"), filename);
    assert(ms && ms->rwops);
    media_start_end(ms, 0, -1);
    media_want_video(ms, dropping ? 1 : 2);
    return ms;
}

static void ready(MediaState *ms) {
    Uint32 deadline = SDL_GetTicks() + 10000;
    for (;;) {
        SDL_LockMutex(ms->lock);
        int done = ms->ready && (ms->video_finished || ms->surface_queue_size >= 2);
        SDL_UnlockMutex(ms->lock);
        if (done) return;
        assert(SDL_GetTicks() < deadline);
        SDL_Delay(1);
    }
}

/* Holding both recursive SDL mutexes excludes background decoding while we
 * prove that the setter preserves every queued entry, pixel buffer and PTS. */
static void change(MediaState *ms, MediaState *next, double speed) {
    SDL_LockMutex(ms->lock);
    SDL_LockMutex(next->lock);
    MediaState *streams[] = { ms, next };
    MediaState states[] = { *ms, *next };
    SurfaceQueueEntry frames[2][FRAMES + 1];
    for (int i = 0; i < 2; i++) {
        int n = 0;
        for (SurfaceQueueEntry *f = streams[i]->surface_queue; f; f = f->next) {
            assert(n < FRAMES + 1);
            frames[i][n++] = *f;
        }
    }
    double pos = media_video_position(ms), queued_pos = media_video_position(next);
    assert(media_set_video_speed(ms, next, speed));
    near(media_video_position(ms), pos);
    near(media_video_position(next), queued_pos);
    for (int i = 0; i < 2; i++) {
        MediaState *s = streams[i], *old = &states[i];
        assert(s->ctx == old->ctx && s->video_context == old->video_context);
        assert(s->thread == old->thread && s->rwops == old->rwops);
        assert(s->surface_queue == old->surface_queue && s->surface_queue_size == old->surface_queue_size);
        assert(s->seek_req == old->seek_req && s->seeking == old->seeking);
        assert(s->needs_decode == old->needs_decode);
        assert(s->tempo_graph == old->tempo_graph);
        int n = 0;
        for (SurfaceQueueEntry *f = s->surface_queue; f; f = f->next) {
            assert(!memcmp(f, &frames[i][n++], sizeof(*f)));
        }
        if (old->video_speed == speed) {
            near(s->video_clock_anchor, old->video_clock_anchor);
            near(s->video_time_anchor, old->video_time_anchor);
        }
    }
    SDL_UnlockMutex(next->lock);
    SDL_UnlockMutex(ms->lock);
}

static void consume(MediaState *ms, int yuv) {
    ready(ms);
    while (media_video_ready(ms)) {
        if (yuv) {
            MediaVideoYUV frame;
            assert(media_read_video_yuv(ms, &frame));
            assert(frame.width == 64 && frame.height == 64);
            media_free_video_yuv(&frame);
        } else {
            SDL_Surface *frame = media_read_video(ms);
            assert(frame);
            SDL_FreeSurface(frame);
        }
    }
    Uint8 silence[1764];
    media_read_audio(ms, silence, sizeof(silence));
}

static void close_movie(MediaState *ms) {
    SDL_Thread *thread = ms->thread;
    media_acquire_video(ms);
    media_close(ms);
    SDL_Delay(1);
    /* A concurrent renderer can finish conversion after a loop/stop without
     * keeping the global mixer locked or losing its decoder context. */
    media_release_video(ms);
    SDL_WaitThread(thread, NULL);
    ms->thread = NULL;
    deallocate_deferred();
}

int main(int argc, char **argv) {
    if (argc != 5 || SDL_Init(0)) return 2;
    media_init(44100, 0, 1);
    SDL_Surface *format = SDL_CreateRGBSurface(0, 1, 1, 32, 0xff, 0xff00, 0xff0000, 0xff000000);
    media_sample_surfaces(format, format);
    MediaState *ms = open_movie(argv[1], atoi(argv[4]));
    MediaState *next = open_movie(argv[1], atoi(argv[4]));
    MediaState *other = open_movie(argv[1], 1);
    MediaState *audio = open_movie(argv[2], 1);
    /* A request arriving before even stream probing must survive startup. */
    assert(media_set_video_speed(ms, next, 0.5));
    media_start(ms); media_start(next); media_start(other); media_start(audio);
    ready(ms); ready(next); ready(other); ready(audio);
    near(ms->video_speed, 0.5);
    near(media_duration(ms), 2);
    change(ms, next, 1);
    int yuv = atoi(argv[3]);
    for (int i = 0; i < 170; i++) { consume(ms, yuv); advance(0.01); }
    near(media_video_position(ms), 1.7);
    change(ms, next, 0.5);
    for (int i = 0; i < 20; i++) { consume(ms, yuv); advance(0.01); }
    near(media_video_position(ms), 1.8);
    change(ms, next, 0.5); /* No reanchor for identical requests. */
    near(other->video_speed, 1);
    near(media_video_position(other), 0); /* A queued clock has not started. */
    near(media_video_position(next), 0);
    media_pause(ms, 1);
    advance(10);
    change(ms, next, 3);
    assert(!media_video_ready(ms));
    near(media_video_position(ms), 1.8);
    advance(10);
    media_pause(ms, 0);
    near(media_video_position(ms), 1.8);
    advance(0.02);
    near(media_video_position(ms), 1.86);
    change(ms, next, 1);
    advance(0.02);
    near(media_video_position(ms), 1.88);
    /* Validate both streams before changing either one. */
    assert(!media_set_video_speed(ms, audio, 0.5));
    near(ms->video_speed, 1);
    assert(!media_set_video_speed(ms, next, NAN));
    assert(!media_set_video_speed(ms, next, 3.01));
    assert(!media_set_video_speed(ms, next, 0));
    change(ms, next, 0.5);
    advance(0.241);
    assert(media_audio_finished(ms));
    near(media_video_position(ms), 2);
    consume(next, yuv);
    advance(0.2);
    near(media_video_position(next), 0.1); /* Latest rate on loop activation. */
    near(media_duration(next), 2);
    close_movie(ms); close_movie(next); close_movie(other); close_movie(audio);
    SDL_FreeSurface(format);
    SDL_DestroyMutex(deallocate_mutex);
    SDL_Quit();
    puts("PASS: continuous clock, preserved decoder/frames, pending, queue, pause, isolation");
    return 0;
}
