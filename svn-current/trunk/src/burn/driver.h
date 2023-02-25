/*
 * For the MAME sound cores
 */

#ifndef DRIVER_H
#define DRIVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if !defined (_WIN32)
 #define __cdecl
#endif

#ifndef INLINE
 #define INLINE __inline static
#endif

#define FBA

typedef unsigned char						UINT8;
typedef signed char 						INT8;
typedef unsigned short						UINT16;
typedef signed short						INT16;
typedef unsigned int						UINT32;
typedef signed int							INT32;
#ifdef _MSC_VER
typedef signed __int64						INT64;
typedef unsigned __int64					UINT64;
#else
__extension__ typedef unsigned long long	UINT64;
__extension__ typedef long long				INT64;
#endif

#endif /* DRIVER_H */
