/*
 * Ifx_Types.h - host stand-in for the iLLD base types
 *
 * Only what Middleware compiles against on the host. Lives on the include path
 * of test/host so the production sources stay untouched; the TriCore build never
 * sees this file.
 */
#ifndef HOST_IFX_TYPES_H
#define HOST_IFX_TYPES_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t     uint8;
typedef uint16_t    uint16;
typedef uint32_t    uint32;
typedef int8_t      sint8;
typedef int16_t     sint16;
typedef int32_t     sint32;
typedef uint64_t    uint64;
typedef int64_t     sint64;
typedef float       float32;
typedef double      float64;

typedef uint8       boolean;
typedef unsigned long uintptr;

#define TRUE        1u
#define FALSE       0u

#ifndef NULL_PTR
#define NULL_PTR    ((void *)0)
#endif

#endif /* HOST_IFX_TYPES_H */
