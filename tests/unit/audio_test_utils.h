#ifndef MACHAUDIO_AUDIO_TEST_UTILS_H
#define MACHAUDIO_AUDIO_TEST_UTILS_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Calculates the Signal-to-Noise Ratio (SNR) in dB between a signal and its noisy version.
 */
static inline double audio_calc_snr(
    int16_t const *restrict const signal,
    int16_t const *restrict const noisy,
    size_t const samples) {
    double signal_pwr = 0.0;
    double noise_pwr  = 0.0;

    for (size_t i = 0; i < samples; ++i) {
        double const s    = (double)signal[i];
        double const n    = (double)noisy[i];
        double const diff = s - n;

        signal_pwr += s * s;
        noise_pwr += diff * diff;
    }

    if (noise_pwr < 1e-10)
        return 100.0; // Perfect reconstruction
    if (signal_pwr < 1e-10)
        return 0.0;

    return 10.0 * log10(signal_pwr / noise_pwr);
}

/**
 * Generates a multi-tone signal.
 */
static inline void audio_gen_multi_tone(
    int16_t *restrict const buf,
    size_t const samples,
    double const *restrict const freqs,
    size_t const num_freqs,
    double const sample_rate_hz,
    double const amplitude) {
    for (size_t i = 0; i < samples; ++i) {
        double const t   = (double)i / sample_rate_hz;
        double       sum = 0.0;
        for (size_t f = 0; f < num_freqs; ++f) {
            sum += sin(2.0 * M_PI * freqs[f] * t);
        }
        buf[i] = (int16_t)(amplitude * (sum / (double)num_freqs));
    }
}

/**
 * Generates a sine wave in L16 format.
 */
static inline void audio_gen_sine(
    int16_t *restrict const buf,
    size_t const samples,
    double const freq_hz,
    double const sample_rate_hz,
    double const amplitude) {
    for (size_t i = 0; i < samples; ++i) {
        double const t = (double)i / sample_rate_hz;
        buf[i]         = (int16_t)(amplitude * sin(2.0 * M_PI * freq_hz * t));
    }
}

/**
 * Generates a frequency sweep (chirp).
 */
static inline void audio_gen_chirp(
    int16_t *restrict const buf,
    size_t const samples,
    double const f0,
    double const f1,
    double const sample_rate_hz,
    double const amplitude) {
    double const duration = (double)samples / sample_rate_hz;
    for (size_t i = 0; i < samples; ++i) {
        double const t = (double)i / sample_rate_hz;
        // Linear chirp: f(t) = f0 + (f1 - f0) * t / (2 * duration)
        // Phase phi(t) = 2 * PI * integral(f(tau) dtau) = 2 * PI * (f0*t + (f1-f0)*t^2 /
        // (2*duration))
        double const phi = 2.0 * M_PI * (f0 * t + (f1 - f0) * t * t / (2.0 * duration));
        buf[i]           = (int16_t)(amplitude * sin(phi));
    }
}

#endif // MACHAUDIO_AUDIO_TEST_UTILS_H
