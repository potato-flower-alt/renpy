/* Compile with src/spectrum.c and -lm; also suitable for ASan/UBSan. */
#include "../src/spectrum.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void near(double actual, double expected) {
    if (fabs(actual - expected) > 0.000001) {
        fprintf(stderr, "expected %.9f, got %.9f\n", expected, actual);
        abort();
    }
}

int main(void) {
    for (int n = 512; n <= 8192; n *= 2) {
        RPS_SpectrumFFT *f = RPS_spectrum_fft_new(n);
        assert(f);
        float *pcm = malloc(2 * n * sizeof(float));
        double *bins = malloc((n / 2 + 1) * sizeof(double));
        double rms, peak;
        for (int mode = 0; mode < 4; mode++) {
            for (int i = 0; i < n; i++) {
                double v = 0.4 * sin(6.28318530717958647692 * 16 * i / n);
                pcm[2 * i] = mode == 3 ? 0 : v;
                pcm[2 * i + 1] = mode == 1 ? -v : (mode == 2 ? 0 : v);
            }
            RPS_spectrum_fft_analyze(f, pcm, bins, &rms, &peak);
            double amplitude = mode >= 2 ? 0.4 / sqrt(2) : 0.4;
            near(bins[16], amplitude);
            near(bins[15], amplitude / 2);
            near(bins[17], amplitude / 2);
            near(rms, amplitude / sqrt(2));
            near(peak, 0.4);
            near(bins[0], 0);
        }
        /* Two tones, with gain above unity, do not clip or aggregate bins. */
        for (int i = 0; i < n; i++) {
            double t = 6.28318530717958647692 * i / n;
            pcm[2 * i] = pcm[2 * i + 1] = 2 * sin(16 * t) + 0.75 * sin(32 * t);
        }
        RPS_spectrum_fft_analyze(f, pcm, bins, &rms, &peak);
        near(bins[16], 2);
        near(bins[32], 0.75);
        near(rms, sqrt((4 + 0.75 * 0.75) / 2));
        assert(peak > 2);
        /* DC and Nyquist have no single-sided factor of two. */
        for (int i = 0; i < n; i++) pcm[2 * i] = pcm[2 * i + 1] = 0.25;
        RPS_spectrum_fft_analyze(f, pcm, bins, &rms, &peak);
        near(bins[0], 0.25);
        near(rms, 0.25);
        for (int i = 0; i < n; i++) pcm[2 * i] = pcm[2 * i + 1] = i & 1 ? -0.25 : 0.25;
        RPS_spectrum_fft_analyze(f, pcm, bins, &rms, &peak);
        near(bins[n / 2], 0.25);
        /* Non-bin-aligned tones leak through Hann but retain their PCM RMS. */
        double square = 0;
        for (int i = 0; i < n; i++) {
            float v = 0.4 * sin(6.28318530717958647692 * 16.5 * i / n);
            pcm[2 * i] = pcm[2 * i + 1] = v;
            square += (double) v * v;
        }
        RPS_spectrum_fft_analyze(f, pcm, bins, &rms, &peak);
        near(rms, sqrt(square / n));
        assert(bins[16] > 0.3 && bins[17] > 0.3 && bins[15] > 0.01);
        /* Filters with invalid output cannot contaminate the snapshot. */
        for (int i = 0; i < 2 * n; i++) pcm[i] = i & 1 ? INFINITY : NAN;
        RPS_spectrum_fft_analyze(f, pcm, bins, &rms, &peak);
        near(rms, 0);
        near(peak, 0);
        for (int k = 0; k <= n / 2; k++) near(bins[k], 0);
        free(pcm);
        free(bins);
        RPS_spectrum_fft_free(f);
        printf("FFT size %d: stereo, antiphase, single-sided, dual tone, DC, Nyquist, leakage, finite PASS\n", n);
    }
    assert(!RPS_spectrum_fft_new(513));
    assert(!RPS_spectrum_fft_new(0));
    puts("All native FFT checks passed.");
    return 0;
}
