#include <string.h>
#include "machaudio/mix.h"
#include "unity.h"

void test_mix_zero_sum(void) {
    int16_t in_a[32];
    int16_t in_b[32];
    int16_t out[32];

    for (int i = 0; i < 32; ++i) {
        in_a[i] = (int16_t)(i * 100);
        in_b[i] = 0;
    }

    mix_l16_avx2(in_a, in_b, out, 32);

    TEST_ASSERT_EQUAL_INT16_ARRAY(in_a, out, 32);
}

void test_mix_standard_addition(void) {
    int16_t in_a[16] = {
        1000,
        2000,
        3000,
        4000,
        5000,
        6000,
        7000,
        8000,
        9000,
        10000,
        11000,
        12000,
        13000,
        14000,
        15000,
        16000};
    int16_t in_b[16] =
        {500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 500};
    int16_t expected[16];
    int16_t out[16];

    for (int i = 0; i < 16; ++i) {
        expected[i] = in_a[i] + in_b[i];
    }

    mix_l16_avx2(in_a, in_b, out, 16);

    TEST_ASSERT_EQUAL_INT16_ARRAY(expected, out, 16);
}

void test_mix_positive_saturation(void) {
    int16_t in_a[16];
    int16_t in_b[16];
    int16_t out[16];

    for (int i = 0; i < 16; ++i) {
        in_a[i] = 30000;
        in_b[i] = 10000;
    }

    mix_l16_avx2(in_a, in_b, out, 16);

    for (int i = 0; i < 16; ++i) {
        TEST_ASSERT_EQUAL_INT16(32767, out[i]);
    }
}

void test_mix_negative_saturation(void) {
    int16_t in_a[16];
    int16_t in_b[16];
    int16_t out[16];

    for (int i = 0; i < 16; ++i) {
        in_a[i] = -30000;
        in_b[i] = -10000;
    }

    mix_l16_avx2(in_a, in_b, out, 16);

    for (int i = 0; i < 16; ++i) {
        TEST_ASSERT_EQUAL_INT16(-32768, out[i]);
    }
}

void test_mix_tail_processing(void) {
    // 20 samples = 16 (SIMD) + 4 (Scalar)
    int16_t in_a[20];
    int16_t in_b[20];
    int16_t expected[20];
    int16_t out[20];

    for (int i = 0; i < 20; ++i) {
        in_a[i]     = 20000;
        in_b[i]     = 20000; // Will saturate
        expected[i] = 32767;
    }

    mix_l16_avx2(in_a, in_b, out, 20);

    TEST_ASSERT_EQUAL_INT16_ARRAY(expected, out, 20);
}
