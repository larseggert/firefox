// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

struct PRFileDesc;

#include "nspr-bindings.h"
#undef NO_DATA  // Defined in hostent.h, conflicts with other code.

/*
** NSPR's version is used to determine the likelihood that the version you
** used to build your component is anywhere close to being compatible with
** what is in the underlying library.
**
** The format of the version string is
**     "<major version>.<minor version>[.<patch level>] [<Beta>]"
*/
#define PR_VERSION "5.00"  // FIXME

#define PR_BEGIN_MACRO do {
#define PR_END_MACRO \
  }                  \
  while (0)

/***********************************************************************
** MACROS:      PR_EXTERN
**              PR_IMPLEMENT
** DESCRIPTION:
**      These are only for externally visible routines and globals.  For
**      internal routines, just use "extern" for type checking and that
**      will not export internal cross-file or forward-declared symbols.
**      Define a macro for declaring procedures return types. We use this to
**      deal with windoze specific type hackery for DLL definitions. Use
**      PR_EXTERN when the prototype for the method is declared. Use
**      PR_IMPLEMENT for the implementation of the method.
**
** Example:
**   in dowhim.h
**     PR_EXTERN( void ) DoWhatIMean( void );
**   in dowhim.c
**     PR_IMPLEMENT( void ) DoWhatIMean( void ) { return; }
**
**
***********************************************************************/
#if defined(WIN32)

#  define PR_EXPORT(__type) extern __declspec(dllexport) __type
#  define PR_EXPORT_DATA(__type) extern __declspec(dllexport) __type
#  define PR_IMPORT(__type) __declspec(dllimport) __type
#  define PR_IMPORT_DATA(__type) __declspec(dllimport) __type

#  define PR_EXTERN(__type) extern __declspec(dllexport) __type
#  define PR_IMPLEMENT(__type) __declspec(dllexport) __type
#  define PR_EXTERN_DATA(__type) extern __declspec(dllexport) __type
#  define PR_IMPLEMENT_DATA(__type) __declspec(dllexport) __type

#  define PR_CALLBACK
#  define PR_CALLBACK_DECL
#  define PR_STATIC_CALLBACK(__x) static __x

#elif defined(__declspec)

#  define PR_EXPORT(__type) extern __declspec(dllexport) __type
#  define PR_EXPORT_DATA(__type) extern __declspec(dllexport) __type
#  define PR_IMPORT(__type) extern __declspec(dllimport) __type
#  define PR_IMPORT_DATA(__type) extern __declspec(dllimport) __type

#  define PR_EXTERN(__type) extern __declspec(dllexport) __type
#  define PR_IMPLEMENT(__type) __declspec(dllexport) __type
#  define PR_EXTERN_DATA(__type) extern __declspec(dllexport) __type
#  define PR_IMPLEMENT_DATA(__type) __declspec(dllexport) __type

#  define PR_CALLBACK
#  define PR_CALLBACK_DECL
#  define PR_STATIC_CALLBACK(__x) static __x

#else /* Unix */

#  define PR_EXPORT(__type) extern __attribute__((visibility("default"))) __type
#  define PR_EXPORT_DATA(__type) \
    extern __attribute__((visibility("default"))) __type
#  define PR_IMPORT(__type) extern __attribute__((visibility("default"))) __type
#  define PR_IMPORT_DATA(__type) \
    extern __attribute__((visibility("default"))) __type

#  define PR_EXTERN(__type) extern __attribute__((visibility("default"))) __type
#  define PR_IMPLEMENT(__type) __attribute__((visibility("default"))) __type
#  define PR_EXTERN_DATA(__type) \
    extern __attribute__((visibility("default"))) __type
#  define PR_IMPLEMENT_DATA(__type) \
    __attribute__((visibility("default"))) __type
#  define PR_CALLBACK
#  define PR_CALLBACK_DECL
#  define PR_STATIC_CALLBACK(__x) static __x

#endif

#define NSPR_API(__type) PR_IMPORT(__type)
#define NSPR_DATA_API(__type) PR_IMPORT_DATA(__type)

#define PR_UINT64(x) UINT64_C(x)
#define PR_INT64(x) INT64_C(x)

#if defined(DEBUG) || defined(FORCE_PR_ASSERT)
#  define PR_ASSERT(_expr) \
    ((_expr) ? ((void)0) : PR_Assert(#_expr, __FILE__, __LINE__))
#  define PR_ASSERT_ARG(_expr) PR_ASSERT(_expr)
#  define PR_NOT_REACHED(_reasonStr) PR_Assert(_reasonStr, __FILE__, __LINE__)
#else
#  define PR_ASSERT(expr) ((void)0)
#  define PR_ASSERT_ARG(expr) ((void)(0 && (expr)))
#  define PR_NOT_REACHED(reasonStr)
#endif

/*
** Compile-time assert. "condition" must be a constant expression.
** The macro can be used only in places where an "extern" declaration is
** allowed.
*/
#define PR_STATIC_ASSERT(condition) \
  extern void pr_static_assert(int arg[(condition) ? 1 : -1])

#define PR_ATOMIC_INCREMENT(val) __sync_add_and_fetch(val, 1)
#define PR_ATOMIC_DECREMENT(val) __sync_sub_and_fetch(val, 1)
#define PR_ATOMIC_SET(val, newval) __sync_lock_test_and_set(val, newval)
#define PR_ATOMIC_ADD(ptr, val) __sync_add_and_fetch(ptr, val)

/***********************************************************************
** MACROS:      PR_ROUNDUP
**              PR_MIN
**              PR_MAX
**              PR_ABS
** DESCRIPTION:
**      Commonly used macros for operations on compatible types.
***********************************************************************/
#define PR_ROUNDUP(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define PR_MIN(x, y) ((x) < (y) ? (x) : (y))
#define PR_MAX(x, y) ((x) > (y) ? (x) : (y))
#define PR_ABS(x) ((x) < 0 ? -(x) : (x))

/***********************************************************************
** MACROS:      PR_BIT
**              PR_BITMASK
** DESCRIPTION:
** Bit masking macros.  XXX n must be <= 31 to be portable
***********************************************************************/
#define PR_BIT(n) ((PRUint32)1 << (n))
#define PR_BITMASK(n) (PR_BIT(n) - 1)

/***********************************************************************
** MACROS:      PR_ARRAY_SIZE
** DESCRIPTION:
**  The number of elements in an array.
***********************************************************************/
#define PR_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/***********************************************************************
** FUNCTION:    PR_NEWZAP()
** DESCRIPTION:
**   PR_NEWZAP() allocates an item of type _struct from the heap
**   and sets the allocated memory to all 0x00.
** INPUTS:  _struct: a data type
** OUTPUTS: pointer to _struct
** RETURN:  pointer to _struct
***********************************************************************/
#define PR_NEWZAP(_struct) ((_struct*)PR_Calloc(1, sizeof(_struct)))

/***********************************************************************
** FUNCTION:    PR_DELETE()
** DESCRIPTION:
**   PR_DELETE() unallocates an object previosly allocated via PR_NEW()
**   or PR_NEWZAP() to the heap.
** INPUTS:  pointer to previously allocated object
** OUTPUTS: the referenced object is returned to the heap
** RETURN:  void
***********************************************************************/
#define PR_DELETE(_ptr) \
  do {                  \
    PR_Free(_ptr);      \
    (_ptr) = NULL;      \
  } while (0)

/*
** sprintf into a fixed size buffer. Guarantees that a NUL is at the end
** of the buffer. Returns the length of the written output, NOT including
** the NUL, or (PRUint32)-1 if an error occurs.
*/
#define PR_snprintf(...) snprintf(__VA_ARGS__)

/*
** sprintf into a PR_MALLOC'd buffer. Return a pointer to the malloc'd
** buffer on success, NULL on failure. Call "PR_smprintf_free" to release
** the memory returned.
*/
#define PR_smprintf(...)          \
  ({                              \
    char* _ptr = NULL;            \
    asprintf(&_ptr, __VA_ARGS__); \
    _ptr;                         \
  })
#define PR_smprintf_free(_ptr) free(_ptr)

/*
** "append" sprintf into a PR_MALLOC'd buffer. "last" is the last value of
** the PR_MALLOC'd buffer. sprintf will append data to the end of last,
** growing it as necessary using realloc. If last is NULL, PR_sprintf_append
** will allocate the initial string. The return value is the new value of
** last for subsequent calls, or NULL if there is a malloc failure.
*/
#define PR_sprintf_append(last, ...)           \
  ({                                           \
    char* _ptr = NULL;                         \
    int len = asprintf(&_ptr, __VA_ARGS__);    \
    if (_ptr) {                                \
      realloc((last), strlen(last) + len + 1); \
      strcat((last), _ptr);                    \
      free(_ptr);                              \
    }                                          \
    (last);                                    \
  })

#define PR_vsnprintf(...) vsnprintf(__VA_ARGS__)

/*
***************************************************************************
** FUNCTION: PR_sscanf
** DESCRIPTION:
**     PR_sscanf() scans the input character string, performs data
**     conversions, and stores the converted values in the data objects
**     pointed to by its arguments according to the format control
**     string.
**
**     PR_sscanf() behaves the same way as the sscanf() function in the
**     Standard C Library (stdio.h), with the following exceptions:
**     - PR_sscanf() handles the NSPR integer and floating point types,
**       such as PRInt16, PRInt32, PRInt64, and PRFloat64, whereas
**       sscanf() handles the standard C types like short, int, long,
**       and double.
**     - PR_sscanf() has no multibyte character support, while sscanf()
**       does.
** INPUTS:
**     const char *buf
**         a character string holding the input to scan
**     const char *fmt
**         the format control string for the conversions
**     ...
**         variable number of arguments, each of them is a pointer to
**         a data object in which the converted value will be stored
** OUTPUTS: none
** RETURNS: PRInt32
**     The number of values converted and stored.
** RESTRICTIONS:
**    Multibyte characters in 'buf' or 'fmt' are not allowed.
***************************************************************************
*/
#define PR_sscanf(...) sscanf(__VA_ARGS__)

/*
** fprintf to a PRFileDesc
*/
// NSPR_API(PRUint32) PR_fprintf(struct PRFileDesc* fd, const char *fmt, ...);
// FIXME
#define PR_fprintf(fd, ...) fprintf((FILE*)(fd), __VA_ARGS__)

#if defined(DEBUG) || defined(FORCE_PR_ASSERT)
#  define PR_ASSERT_CURRENT_THREAD_OWNS_LOCK(lock) \
    PR_AssertCurrentThreadOwnsLock(lock)
#else
#  define PR_ASSERT_CURRENT_THREAD_OWNS_LOCK(...)
#endif

#define PR_INIT_STATIC_CLIST(_l) {(_l), (_l)}

#if defined(DEBUG) || defined(FORCE_PR_LOG)
/*
** Log something.
**    "module" is the address of a PRLogModuleInfo structure
**    "level" is the desired logging level
**    "args" is a variable length list of arguments to print, in the following
**       format:  ("printf style format string", ...)
*/
#  define PR_LogPrint(...)                             \
    do {                                               \
      char* _log_str = NULL;                           \
      if (asprintf(&_log_str, __VA_ARGS__) < 0) break; \
      PR_LogPrintRust(_log_str);                       \
      free(_log_str);                                  \
    } while (0)

// See https://stackoverflow.com/a/62984543/2240756 for the DEPAREN trick
#  define DEPAREN(X) ESC(ISH X)
#  define ISH(...) ISH __VA_ARGS__
#  define ESC(...) ESC_(__VA_ARGS__)
#  define ESC_(...) VAN##__VA_ARGS__
#  define VANISH
#  define PR_LOG(_module, _level, _args)  \
    do {                                  \
      if ((_module)->level >= (_level)) { \
        PR_LogPrint(DEPAREN(_args));      \
      }                                   \
    } while (0)
#else
#  define PR_LOG(...)
#  define PR_LogPrint(...)
#endif

#define PR_PRIORITY_LAST PR_PRIORITY_URGENT  // FIXME

#ifdef __cplusplus
#  define PR_BEGIN_EXTERN_C extern "C" {
#  define PR_END_EXTERN_C }
#else
#  define PR_BEGIN_EXTERN_C
#  define PR_END_EXTERN_C
#endif
