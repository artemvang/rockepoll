/**
 * copyright (c) 2014 joseph werle <joseph.werle@gmail.com>
 */


#ifndef MURMURHASH_H
#define MURMURHASH_H

#include <stdint.h>


uint32_t murmurhash (const char * key, uint32_t size, uint32_t seed);

#endif