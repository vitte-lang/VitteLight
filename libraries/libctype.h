#ifndef VL_LIB_CTYPE_H
#define VL_LIB_CTYPE_H

#include "core/ctype.h"

#ifndef isascii
#define isascii vt_isascii
#endif
#ifndef isdigit
#define isdigit vt_isdigit
#endif
#ifndef isxdigit
#define isxdigit vt_isxdigit
#endif
#ifndef isalpha
#define isalpha vt_isalpha
#endif
#ifndef isalnum
#define isalnum vt_isalnum
#endif
#ifndef islower
#define islower vt_islower
#endif
#ifndef isupper
#define isupper vt_isupper
#endif
#ifndef isblank
#define isblank vt_isblank
#endif
#ifndef isspace
#define isspace vt_isspace
#endif
#ifndef iscntrl
#define iscntrl vt_iscntrl
#endif
#ifndef isprint
#define isprint vt_isprint
#endif
#ifndef isgraph
#define isgraph vt_isgraph
#endif
#ifndef ispunct
#define ispunct vt_ispunct
#endif
#ifndef tolower
#define tolower vt_tolower
#endif
#ifndef toupper
#define toupper vt_toupper
#endif
#ifndef toascii
#define toascii vt_toascii
#endif

#endif /* VL_LIB_CTYPE_H */
