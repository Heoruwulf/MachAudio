#include "machaudio/arena.h"
#include "unity.h"

#include <stdlib.h>

void test_arena_init(void) {
    Arena   arena;
    uint8_t buf[1024];
    arena_init(&arena, buf, sizeof(buf), "test");

    TEST_ASSERT_EQUAL_PTR(buf, arena.buf);
    TEST_ASSERT_EQUAL(sizeof(buf), arena.size);
    TEST_ASSERT_EQUAL(0, arena.curr);
    TEST_ASSERT_EQUAL_STRING("test", arena.name);
}

void test_arena_alloc(void) {
    Arena   arena;
    uint8_t buf[1024];
    arena_init(&arena, buf, sizeof(buf), "test");

    void *ptr1 = arena_alloc(&arena, 10);
    TEST_ASSERT_NOT_NULL(ptr1);
    TEST_ASSERT_EQUAL_PTR(buf, ptr1);
    TEST_ASSERT_EQUAL(10, arena.curr);

    void *ptr2 = arena_alloc(&arena, 10);
    TEST_ASSERT_NOT_NULL(ptr2);
    // 10 aligned up to 8 is 16
    TEST_ASSERT_EQUAL_PTR(&buf[16], ptr2);
    TEST_ASSERT_EQUAL(26, arena.curr);
}

void test_arena_alignment(void) {
    Arena   arena;
    uint8_t buf[1024];
    arena_init(&arena, buf, sizeof(buf), "test");

    arena_alloc(&arena, 1); // 1 byte
    void *ptr = arena_alloc(&arena, 8);

    // ptr should be at offset 8
    TEST_ASSERT_EQUAL(0, (uintptr_t)ptr % 8);
    TEST_ASSERT_EQUAL_PTR(&buf[8], ptr);
}

void test_arena_oom(void) {
    Arena   arena;
    uint8_t buf[16];
    arena_init(&arena, buf, sizeof(buf), "test");

    void *ptr1 = arena_alloc(&arena, 10);
    TEST_ASSERT_NOT_NULL(ptr1);

    void *ptr2 = arena_alloc(&arena, 10);
    TEST_ASSERT_NULL(ptr2);
}

void test_arena_reset(void) {
    Arena   arena;
    uint8_t buf[1024];
    arena_init(&arena, buf, sizeof(buf), "test");

    arena_alloc(&arena, 100);
    TEST_ASSERT_EQUAL(100, arena.curr);

    arena_reset(&arena);
    TEST_ASSERT_EQUAL(0, arena.curr);

    void *ptr = arena_alloc(&arena, 10);
    TEST_ASSERT_EQUAL_PTR(buf, ptr);
}
