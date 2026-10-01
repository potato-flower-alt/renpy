/* Deterministic movie decoder test. Build with the same command/libraries as
 * audio_tempo_driver.c. The clock advances only after the decoder is ready. */
#include <stdatomic.h>
#include <stdint.h>
static _Atomic int64_t clock_ticks = 100000000;
#define av_gettime_relative test_clock_now
#include "../src/ffmedia.c"
int64_t test_clock_now(void) { return atomic_load(&clock_ticks); }
static double current_time = 100;
static void advance_clock(double delta) {
    current_time += delta;
    atomic_store(&clock_ticks, llround(current_time * 1000000));
}

int main(int argc, char **argv) {
    if (argc < 6 || SDL_Init(0)) return 2;
    media_init(44100, 0, 1);
    SDL_Surface *format = SDL_CreateRGBSurface(0, 1, 1, 32, 0xff, 0xff00, 0xff0000, 0xff000000);
    media_sample_surfaces(format, format);
    MediaState *ms = media_open(SDL_RWFromFile(argv[1], "rb"), argv[1]);
    if (!ms || !ms->rwops) return 3;
    media_start_end(ms, atof(argv[3]), atof(argv[4]));
    media_set_playback_speed(ms, atof(argv[2]));
    media_want_video(ms, 2);
    current_time = 100.0;
    media_start(ms);
    int yuv = atoi(argv[5]) & 1;

    for (int pass = 5; pass < argc; pass++) {
        if (pass > 5) media_seek(ms, atof(argv[pass]));
        int64_t samples = 0;
        int frames = 0;
        int paused = 0, duration_cleared = 0;
        double first = -1, last = -1;
        double origin = current_time;
        Uint32 deadline = SDL_GetTicks() + 30000;
        while (1) {
            SDL_LockMutex(ms->lock);
            int wait = !ms->ready || ms->seek_req || ms->seeking ||
                (!ms->audio_finished && ms->audio_queue_samples < 4096) ||
                (!ms->video_finished && ms->surface_queue_size < 2);
            SDL_UnlockMutex(ms->lock);
            if (wait) {
                if (SDL_GetTicks() > deadline) return 4;
                SDL_Delay(1);
                continue;
            }
            if ((atoi(argv[5]) & 2) && !duration_cleared) {
                SDL_LockMutex(ms->lock);
                ms->audio_duration = -1;
                ms->total_duration = 0;
                SDL_UnlockMutex(ms->lock);
                duration_cleared = 1;
            }
            if (frames == 1 && !paused) {
                media_pause(ms, 1);
                advance_clock(5);
                origin += 5;
                if (media_video_ready(ms)) return 5;
                media_pause(ms, 0);
                paused = 1;
            }
            while (media_video_ready(ms)) {
                int got;
                if (yuv) {
                    MediaVideoYUV frame;
                    got = media_read_video_yuv(ms, &frame);
                    media_free_video_yuv(&frame);
                } else {
                    SDL_Surface *frame = media_read_video(ms);
                    got = frame != NULL;
                    SDL_FreeSurface(frame);
                }
                if (!got) return 6;
                if (first < 0) first = current_time - origin;
                last = current_time - origin;
                frames++;
            }
            short pcm[882]; /* 10ms stereo. */
            int n = media_read_audio(ms, (Uint8 *) pcm, sizeof(pcm));
            samples += n / BPS;
            if (!n && media_audio_finished(ms)) break;
            advance_clock(0.01);
            if (current_time - origin > 30 || SDL_GetTicks() > deadline) return 7;
        }
        double position = media_audio_position(ms);
        printf("RESULT %lld %d %.6f %.6f %.6f\n", (long long) samples, frames, first, last, position);
    }
    SDL_Thread *thread = ms->thread;
    media_close(ms);
    SDL_WaitThread(thread, NULL);
    ms->thread = NULL;
    deallocate_deferred();
    SDL_FreeSurface(format);
    SDL_DestroyMutex(deallocate_mutex);
    SDL_Quit();
    return 0;
}
