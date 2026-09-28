#pragma once
// PC shim for ESP-IDF's heap_caps: the simulator has one heap, so the caps are ignored.
#include <cstdlib>
#define MALLOC_CAP_SPIRAM   (1u << 10)
#define MALLOC_CAP_8BIT     (1u << 2)
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_DEFAULT  (1u << 12)
static inline void* heap_caps_malloc(size_t n, unsigned) { return std::malloc(n); }
static inline void* heap_caps_calloc(size_t n, size_t s, unsigned) { return std::calloc(n, s); }
static inline void* heap_caps_realloc(void* p, size_t n, unsigned) { return std::realloc(p, n); }
static inline void  heap_caps_free(void* p) { std::free(p); }
static inline size_t heap_caps_get_free_size(unsigned) { return 64u << 20; }
static inline size_t heap_caps_get_largest_free_block(unsigned) { return 32u << 20; }
