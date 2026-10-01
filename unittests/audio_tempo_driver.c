/* Offline regression driver for the real decoder, without an audio device.
 * Including the implementation permits deterministic queue/seek inspection
 * without adding a test-only API to Ren'Py. */
#include "../src/ffmedia.c"

/* An EOF stream is retained for backward seeking. Repeated reads, including
 * underruns while its decoder is working, must not turn into wakeup polling. */
static int check_tempo_idle(void) {
    if (SDL_Init(0)) return 3;
    MediaState ms = { 0 };
    ms.lock = SDL_CreateMutex();
    ms.cond = SDL_CreateCond();
    ms.ready = 1;
    ms.playback_speed = 1.5;
    ms.audio_end = -1;
    short pcm[64];
    int result = 0;

    for (int finished = 0; finished <= 1; finished++) {
        ms.audio_finished = finished;
        for (int i = 0; i < 100; i++) {
            if (media_read_audio(&ms, (Uint8 *) pcm, sizeof(pcm)) || ms.needs_decode) {
                fprintf(stderr, "Empty tempo read requested decode (finished=%d).\n", finished);
                result = 7;
                goto done;
            }
        }
    }

    /* Consuming buffered output requests a refill only before decoder EOF. */
    for (int finished = 0; finished <= 1; finished++) {
        AVFrame *frame = tempo_frame();
        if (!frame) { result = 4; goto done; }
        frame->nb_samples = 32;
        if (av_frame_get_buffer(frame, 0) < 0) {
            av_frame_free(&frame);
            result = 4;
            goto done;
        }
        memset(frame->data[0], 0, sizeof(pcm));
        enqueue_frame(&ms.audio_queue, frame);
        ms.audio_queue_samples = 32;
        ms.audio_finished = finished;
        ms.needs_decode = 0;
        if (media_read_audio(&ms, (Uint8 *) pcm, sizeof(pcm)) != sizeof(pcm) ||
                ms.needs_decode != !finished) {
            fprintf(stderr, "Incorrect tempo refill request (finished=%d).\n", finished);
            result = 8;
            goto done;
        }
    }

    ms.needs_decode = 0;
    media_seek(&ms, 0.25);
    if (!ms.seek_req || !ms.needs_decode) result = 9;

done:
    SDL_DestroyCond(ms.cond);
    SDL_DestroyMutex(ms.lock);
    SDL_Quit();
    return result;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--idle")) return check_tempo_idle();
    if (argc < 5) return 2;
    if (SDL_Init(0)) return 3;
    media_init(44100, 0, 1);
    MediaState *ms = media_open(SDL_RWFromFile(argv[1], "rb"), argv[1]);
    if (!ms || !ms->rwops) return 4;
    media_start_end(ms, atof(argv[3]), atof(argv[4]));
    media_set_playback_speed(ms, atof(argv[2]));
    media_start(ms);

    for (int pass = 4; pass < argc; pass++) {
        if (pass > 4) media_seek(ms, atof(argv[pass]));
        int64_t samples = 0;
        Uint32 deadline = SDL_GetTicks() + 30000;
        while (1) {
            SDL_LockMutex(ms->lock);
            int wait = !ms->ready || ms->seek_req || ms->seeking ||
                (!ms->audio_finished && ms->audio_queue_samples < 2048);
            SDL_UnlockMutex(ms->lock);
            if (wait) {
                if (SDL_GetTicks() > deadline) return 5;
                SDL_Delay(1);
                continue;
            }
            short pcm[4096];
            int n = media_read_audio(ms, (Uint8 *) pcm, sizeof(pcm));
            if (n) {
                fwrite(pcm, 1, n, stdout);
                samples += n / BPS;
            } else if (media_audio_finished(ms)) {
                break;
            } else {
                if (SDL_GetTicks() > deadline) return 6;
                SDL_Delay(1);
            }
        }
        fprintf(stderr, "RESULT %lld %.6f %.6f\n", (long long) samples,
            ms->playback_speed == 1.0 ? ms->skip + (double) ms->audio_read_samples / audio_sample_rate :
            media_audio_position(ms), media_duration(ms));
    }
    SDL_Thread *thread = ms->thread;
    media_close(ms);
    SDL_WaitThread(thread, NULL);
    ms->thread = NULL;
    deallocate_deferred();
    SDL_DestroyMutex(deallocate_mutex);
    SDL_Quit();
    return 0;
}
