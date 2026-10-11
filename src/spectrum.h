/* Copyright 2026. Distributed under the MIT license (see renpysound_core.c). */
#ifndef RENPY_SPECTRUM_H
#define RENPY_SPECTRUM_H

#include <stdint.h>

enum { RPS_SPECTRUM_WARMING, RPS_SPECTRUM_PLAYING, RPS_SPECTRUM_PAUSED,
       RPS_SPECTRUM_STARVED, RPS_SPECTRUM_STOPPED };

typedef struct {
    uint64_t generation;
    uint64_t count;
    int sample_rate;
    int fft_size;
    int state;
    double sample_time;
    double position;
} RPS_SpectrumSnapshot;

typedef struct RPS_SpectrumFFT RPS_SpectrumFFT;
RPS_SpectrumFFT *RPS_spectrum_fft_new(int size);
void RPS_spectrum_fft_free(RPS_SpectrumFFT *fft);
void RPS_spectrum_fft_analyze(RPS_SpectrumFFT *fft, const float *pcm,
                              double *magnitudes, double *rms, double *peak);

#endif
