/* LAME's config.h for FastPlay's build of its encoder library (CMakeLists.txt),
   in place of the one its configure script would write. The same on every
   system FastPlay builds on: a C99 compiler with the standard headers. */
#ifndef LAME_CONFIG_H
#define LAME_CONFIG_H

#define STDC_HEADERS 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_MEMCPY 1
#define HAVE_STRCHR 1
#define PACKAGE "lame"
#define PROTOTYPES 1
#define USE_FAST_LOG 1
#define LAME_LIBRARY_BUILD

#include <stdint.h>

typedef long double ieee854_float80_t;
typedef double ieee754_float64_t;
typedef float ieee754_float32_t;

#endif
