// Comprehensive system header stubs for gcc 4.8 preprocessing
#ifndef GCC48_SYSTEM_STUBS_H
#define GCC48_SYSTEM_STUBS_H

// Basic types
typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef long intptr_t;
typedef unsigned long uintptr_t;
typedef long ssize_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;
typedef int int32_t;
typedef long int64_t;
typedef unsigned char uint8_t;
typedef char int8_t;
typedef unsigned short uint16_t;
typedef short int16_t;

// C library stubs
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
char *strcpy(char *, const char *);
char *strcat(char *, const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
int sprintf(char *, const char *, ...);
int fprintf(void *, const char *, ...);
int printf(const char *, ...);
void abort(void);
void exit(int);

// FILE type
typedef struct __FILE FILE;
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

// GCC internal macros
#define NULL ((void*)0)
#define GTY(x)
#define ATTRIBUTE_UNUSED __attribute__((unused))
#define ATTRIBUTE_NORETURN __attribute__((noreturn))
#define gcc_assert(x) ((void)(x))
#define gcc_checking_assert(x) ((void)0)
#define gcc_unreachable() __builtin_unreachable()
#define CONST_CAST2(TO,FROM,X) ((TO)(X))
#define CONST_CAST(T,X) ((T)(X))
#define STATIC_ASSERT(x)

// Host config
typedef long HOST_WIDE_INT;
typedef unsigned long HOST_WIDEST_INT;
#define HOST_BITS_PER_WIDE_INT 64
#define HOST_BITS_PER_WIDEST_INT 64
#define HOST_BITS_PER_LONG 64
#define HOST_BITS_PER_INT 32
typedef unsigned int hashval_t;

// Statistics stubs
#define GATHER_STATISTICS 0
#define CXX_MEM_STAT_INFO
#define MEM_STAT_DECL
#define ALONE_MEM_STAT_DECL
#define PASS_MEM_STAT
#define ALONE_PASS_MEM_STAT
#define FINAL_PASS_MEM_STAT
#define MEM_STAT_INFO
#define ALONE_MEM_STAT_INFO
#define ALONE_CXX_MEM_STAT_INFO
#define CHECKING_P 0

// Prevent real system header inclusion
#define _STDLIB_H 1
#define _STRING_H 1
#define _STDIO_H 1
#define _STDDEF_H 1
#define _STDINT_H 1
#define _STDARG_H 1
#define _SYS_TYPES_H 1

// GCC config.h / system.h stubs
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define GCC_VERSION 4008

// va_list
typedef __builtin_va_list va_list;
#define va_start(ap, param) __builtin_va_start(ap, param)
#define va_end(ap) __builtin_va_end(ap)
#define va_arg(ap, type) __builtin_va_arg(ap, type)

// offsetof
#define offsetof(TYPE, MEMBER) __builtin_offsetof(TYPE, MEMBER)

#endif
#define ATTRIBUTE_NONNULL(n)
#define ATTRIBUTE_PRINTF(m,n)
#define ATTRIBUTE_MALLOC
#define ENUM_BITFIELD(TYPE) unsigned int
#define XCNEW(T) ((T*)calloc(1, sizeof(T)))
#define XNEW(T) ((T*)malloc(sizeof(T)))
#define XNEWVEC(T,N) ((T*)calloc((N), sizeof(T)))
#define XRESIZEVEC(T,P,N) ((T*)realloc((P), (N)*sizeof(T)))
#define XDELETEVEC(P) free(P)
#define ATTRIBUTE_PRINTF_1
#define ATTRIBUTE_PRINTF_2
#define ATTRIBUTE_PRINTF_3
#define ATTRIBUTE_PRINTF_4
