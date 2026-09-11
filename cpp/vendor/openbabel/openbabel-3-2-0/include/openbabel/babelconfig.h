#ifndef OPENBABEL_BABELCONFIG_H
#define OPENBABEL_BABELCONFIG_H

#define BABEL_DATADIR "."
#define BABEL_VERSION "3.2.0"

#define OB_VERSION_CHECK(major, minor, patch) ((major << 16) | (minor << 8) | (patch))
#define OB_VERSION OB_VERSION_CHECK(3, 2, 0)

#define MODULE_EXTENSION ".obf"

#define OB_EXPORT
#define OB_IMPORT
#define OB_HIDDEN

#define OB_DEPRECATED
#define OB_DEPRECATED_MSG(msg)

#ifndef OB_EXTERN
#define OB_EXTERN OB_IMPORT extern
#endif
#ifndef OBAPI
#define OBAPI OB_IMPORT
#endif
#ifndef OBCOMMON
#define OBCOMMON OB_IMPORT
#endif
#ifndef OBCONV
#define OBCONV OB_IMPORT
#endif
#ifndef OBERROR
#define OBERROR OB_IMPORT
#endif
#ifndef OBFPRT
#define OBFPRT OB_IMPORT
#endif
#ifndef OBFPTR
#define OBFPTR OB_IMPORT
#endif
#ifndef OBMCDL
#define OBMCDL OB_IMPORT
#endif
#ifndef OBDEPICT
#define OBDEPICT OB_IMPORT
#endif

#define HAVE_CONIO_H 1
#if !defined(_WIN32)
#define HAVE_SYS_TIME_H 1
#endif
#define HAVE_TIME_H 1
#define HAVE_SSTREAM 1
#define HAVE_CLOCK_T 1
#define HAVE_RINT 1
#define HAVE_SNPRINTF 1
#if defined(_WIN32)
#define HAVE_STRCASECMP 0
#define HAVE_STRNCASECMP 0
#else
#define HAVE_STRCASECMP 1
#define HAVE_STRNCASECMP 1
#endif
#define HAVE_ISFINITE 1

#define OB_SHARED_PTR_IMPLEMENTATION std::shared_ptr
#define OB_SHARED_PTR_HEADER <memory>

#define OB_MODULE_PATH "."

#if defined(_WIN32)
#ifndef HAVE_ISFINITE
#define isfinite _finite
#define HAVE_ISFINITE 1
#endif

#ifndef HAVE_SNPRINTF
#define snprintf _snprintf
#define HAVE_SNPRINTF 1
#endif

#ifndef HAVE_STRCASECMP
#define strcasecmp _stricmp
#define HAVE_STRCASECMP 1
#endif

#ifndef HAVE_STRNCASECMP
#define strncasecmp _strnicmp
#define HAVE_STRNCASECMP 1
#endif
#endif

#endif
