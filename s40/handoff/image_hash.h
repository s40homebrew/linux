/* SPDX-License-Identifier: MIT */
#ifndef LINUX6303_IMAGE_HASH_H
#define LINUX6303_IMAGE_HASH_H
#include <stddef.h>
#include <stdint.h>
/* FNV-1a detects accidental transfer/RAM corruption; not authentication. */
static inline uint32_t linux6303_image_hash(const void *image, size_t bytes) {
    const volatile uint8_t *p = image;
    uint32_t hash = 2166136261u;
    while (bytes--)
        hash = (hash ^ *p++) * 16777619u;
    return hash;
}
#endif
