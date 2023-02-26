#ifndef __PORT_TYPEDEFS_H
#define __PORT_TYPEDEFS_H

#include <stdint.h>
#include <wchar.h>

#include "inp_keys.h"
#define TCHAR char
#define _strnicmp(s1, s2, n) strncasecmp(s1, s2, n)

#ifdef _MSC_VER
#include <tchar.h>
#define strncasecmp(s1, s2, n) _strnicmp(s1, s2, n)
#define strcasecmp(x, y) _stricmp(x, y)
#define snprintf _snprintf
#else
#define _stricmp(x, y) strcasecmp(x,y)

typedef struct { int x, y, width, height; } RECT;
#undef __cdecl
#define __cdecl
#endif

#undef __fastcall
#undef _fastcall
#define __fastcall			/*what does this correspond to?*/
#define _fastcall			/*same as above - what does this correspond to?*/

/* for Windows / Xbox 360 (below VS2010) - typedefs for missing stdint.h types such as uintptr_t?*/

extern void InpDIPSWResetDIPs (void);

#endif
