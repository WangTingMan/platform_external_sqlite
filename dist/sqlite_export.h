#pragma once

#if defined(WIN32) || defined(_MSC_VER)

#if defined(LIBSQLITE_EXPORTS_IMPL)
#define LIBSQLITE_EXPORTS __declspec(dllexport)
#else
#define LIBSQLITE_EXPORTS __declspec(dllimport)
#endif  // defined(LIBSQLITE_EXPORTS_IMPL)

#else  // defined(WIN32)
#if defined(LIBSQLITE_EXPORTS_IMPL)
#define LIBSQLITE_EXPORTS __attribute__((visibility("default")))
#else
#define LIBSQLITE_EXPORTS
#endif  // defined(LIBSQLITE_EXPORTS_IMPL)
#endif

#define SQLITE_API LIBSQLITE_EXPORTS

