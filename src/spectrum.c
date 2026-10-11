/* Copyright 2026. Distributed under the MIT license (see renpysound_core.c). */
#include "spectrum.h"
#include <math.h>
#include <stdlib.h>

/* A small, dependency-free radix-2 FFT. Plans and scratch storage are reused;
 * this file knows nothing about audio devices, locks, or Python. */
struct RPS_SpectrumFFT {
    int size;
    int *reverse;
    double *window, *cosine, *sine, *real, *imag, *power;
};

void RPS_spectrum_fft_free(RPS_SpectrumFFT *f) {
    if (!f) return;
    free(f->reverse);
    free(f->window);
    free(f);
}

RPS_SpectrumFFT *RPS_spectrum_fft_new(int size) {
    if (size < 512 || size > 8192 || (size & (size - 1))) return NULL;
    RPS_SpectrumFFT *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->size = size;
    f->reverse = malloc(size * sizeof(int));
    f->window = malloc(6 * size * sizeof(double));
    if (!f->reverse || !f->window) {
        RPS_spectrum_fft_free(f);
        return NULL;
    }
    f->cosine = f->window + size;
    f->sine = f->cosine + size;
    f->real = f->sine + size;
    f->imag = f->real + size;
    f->power = f->imag + size;
    for (int i = 0; i < size; i++) {
        double angle = 6.28318530717958647692 * i / size;
        f->window[i] = 0.5 - 0.5 * cos(angle);
        f->cosine[i] = cos(angle);
        f->sine[i] = -sin(angle);
        int index = i, reverse = 0;
        for (int n = size; n > 1; n >>= 1) {
            reverse = (reverse << 1) | (index & 1);
            index >>= 1;
        }
        f->reverse[i] = reverse;
    }
    return f;
}

void RPS_spectrum_fft_analyze(RPS_SpectrumFFT *f, const float *pcm,
                             double *magnitudes, double *rms, double *peak) {
    int n = f->size;
    double sum = 0, maximum = 0;
    for (int channel = 0; channel < 2; channel++) {
        for (int i = 0; i < n; i++) {
            double v = pcm[2 * i + channel];
            if (!isfinite(v)) v = 0;
            sum += v * v;
            if (fabs(v) > maximum) maximum = fabs(v);
            f->real[f->reverse[i]] = v * f->window[i];
            f->imag[f->reverse[i]] = 0;
        }
        for (int width = 2; width <= n; width <<= 1) {
            int half = width / 2;
            for (int base = 0; base < n; base += width) {
                for (int j = 0; j < half; j++) {
                    int a = base + j, b = a + half, t = j * n / width;
                    double real = f->real[b] * f->cosine[t] - f->imag[b] * f->sine[t];
                    double imag = f->real[b] * f->sine[t] + f->imag[b] * f->cosine[t];
                    f->real[b] = f->real[a] - real;
                    f->imag[b] = f->imag[a] - imag;
                    f->real[a] += real;
                    f->imag[a] += imag;
                }
            }
        }
        for (int k = 0; k <= n / 2; k++) {
            double power = f->real[k] * f->real[k] + f->imag[k] * f->imag[k];
            if (!channel) f->power[k] = power;
            else {
                double factor = (k == 0 || k == n / 2) ? 1.0 : 2.0;
                magnitudes[k] = factor / (n * 0.5) * sqrt((f->power[k] + power) * 0.5);
            }
        }
    }
    *rms = sqrt(sum / (2 * n));
    *peak = maximum;
}
