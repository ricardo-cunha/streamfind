#pragma once

#if defined(_WIN32) && defined(STREAMFIND_CORE_BUILDING_SHARED)
#  define STREAMFIND_CORE_API __declspec(dllexport)
#elif defined(_WIN32) && defined(STREAMFIND_CORE_USING_SHARED)
#  define STREAMFIND_CORE_API __declspec(dllimport)
#else
#  define STREAMFIND_CORE_API
#endif

#if defined(_WIN32) && defined(STREAMFIND_DOMAIN_BUILDING_SHARED)
#  define STREAMFIND_DOMAIN_API __declspec(dllexport)
#elif defined(_WIN32) && defined(STREAMFIND_DOMAIN_USING_SHARED)
#  define STREAMFIND_DOMAIN_API __declspec(dllimport)
#else
#  define STREAMFIND_DOMAIN_API
#endif

#if defined(_WIN32) && defined(STREAMFIND_SDK_BUILDING_SHARED)
#  define STREAMFIND_SDK_API __declspec(dllexport)
#elif defined(_WIN32) && defined(STREAMFIND_SDK_USING_SHARED)
#  define STREAMFIND_SDK_API __declspec(dllimport)
#else
#  define STREAMFIND_SDK_API
#endif
