/* Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5VK_SHADER_CACHE_H
#define PS5VK_SHADER_CACHE_H
#include "psbc_compile.h"

struct ps5vk_shader_cache_key {
   char path[1024];
   unsigned char digest[32];
};
bool ps5vk_shader_cache_key(const uint32_t *words, size_t size,
                            const PsbcCompileOptions *options,
                            struct ps5vk_shader_cache_key *key);
struct nir_shader;
bool ps5vk_shader_cache_nir_key(const struct nir_shader *nir,
                                const PsbcCompileOptions *options,
                                struct ps5vk_shader_cache_key *key);
bool ps5vk_shader_cache_load(const struct ps5vk_shader_cache_key *key, PsbcShaderOutput *output);
void ps5vk_shader_cache_store(const struct ps5vk_shader_cache_key *key,
                              const PsbcShaderOutput *output);
#endif
