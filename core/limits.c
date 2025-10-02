/* ============================================================================
 * limits.c — Portable limits inspection for VitteLight Runtime (C11)
 * ============================================================================
 */

#include <stdio.h>
#include <limits.h>
#include <float.h>
#include <stdint.h>

void print_integer_limits(void) {
    printf("Integer type limits:\n");
    printf("  CHAR_BIT      = %d\n", CHAR_BIT);
    printf("  SCHAR_MIN     = %d\n", SCHAR_MIN);
    printf("  SCHAR_MAX     = %d\n", SCHAR_MAX);
    printf("  UCHAR_MAX     = %u\n", UCHAR_MAX);
    printf("  CHAR_MIN      = %d\n", CHAR_MIN);
    printf("  CHAR_MAX      = %d\n", CHAR_MAX);
    printf("  SHRT_MIN      = %d\n", SHRT_MIN);
    printf("  SHRT_MAX      = %d\n", SHRT_MAX);
    printf("  USHRT_MAX     = %u\n", USHRT_MAX);
    printf("  INT_MIN       = %d\n", INT_MIN);
    printf("  INT_MAX       = %d\n", INT_MAX);
    printf("  UINT_MAX      = %u\n", UINT_MAX);
    printf("  LONG_MIN      = %ld\n", LONG_MIN);
    printf("  LONG_MAX      = %ld\n", LONG_MAX);
    printf("  ULONG_MAX     = %lu\n", ULONG_MAX);
    printf("  LLONG_MIN     = %lld\n", LLONG_MIN);
    printf("  LLONG_MAX     = %lld\n", LLONG_MAX);
    printf("  ULLONG_MAX    = %llu\n", ULLONG_MAX);
}

void print_float_limits(void) {
    printf("\nFloating-point type limits:\n");
    printf("  FLT_MIN       = %e\n", FLT_MIN);
    printf("  FLT_MAX       = %e\n", FLT_MAX);
    printf("  DBL_MIN       = %e\n", DBL_MIN);
    printf("  DBL_MAX       = %e\n", DBL_MAX);
    printf("  LDBL_MIN      = %Le\n", LDBL_MIN);
    printf("  LDBL_MAX      = %Le\n", LDBL_MAX);
    printf("  FLT_EPSILON   = %e\n", FLT_EPSILON);
    printf("  DBL_EPSILON   = %e\n", DBL_EPSILON);
    printf("  LDBL_EPSILON  = %Le\n", LDBL_EPSILON);
}

#ifdef LIMITS_MAIN
int main(void) {
    print_integer_limits();
    print_float_limits();
    return 0;
}
#endif
