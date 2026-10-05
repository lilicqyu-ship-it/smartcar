#ifndef STUB_IFX_TYPES_H
#define STUB_IFX_TYPES_H
/* Minimal stand-in for the iLLD Ifx_Types.h, just enough for rt/servo.c. */
typedef signed char        sint8;
typedef unsigned char      uint8;
typedef signed short       sint16;
typedef unsigned short     uint16;
typedef signed int         sint32;
typedef unsigned int       uint32;
typedef signed long long   sint64;
typedef unsigned long long uint64;
typedef float              float32;
typedef double             float64;
typedef unsigned char      boolean;
#ifndef TRUE
#define TRUE  1u
#define FALSE 0u
#endif
#endif /* STUB_IFX_TYPES_H */
