// Copyright (c) 2011 Baidu.com, Inc. All Rights Reserved
// Thread-local utilities

// Author: Ge,Jun (gejun@baidu.com)
// Date: Mon. Nov 7 14:47:36 CST 2011

#ifndef BRPC_BASE_THREAD_LOCAL_H
#define BRPC_BASE_THREAD_LOCAL_H

#include <new>                      // std::nothrow
#include <cstddef>                  // NULL
#include "base/macros.h"            

// Provide thread_local keyword (for primitive types) before C++11
// DEPRECATED: define this keyword before C++11 might make the variable ABI
// incompatible between C++11 and C++03
#if !defined(thread_local) &&                                           \
    (__cplusplus < 201103L ||                                           \
     (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__) < 40800)
// GCC supports thread_local keyword of C++11 since 4.8.0
#ifdef _MSC_VER
// WARNING: don't use this macro in C++03
#define thread_local __declspec(thread)
#else
// WARNING: don't use this macro in C++03
#define thread_local __thread
#endif  // _MSC_VER
#endif

#ifdef _MSC_VER
#define BAIDU_THREAD_LOCAL __declspec(thread)
#else
#define BAIDU_THREAD_LOCAL __thread
#endif  // _MSC_VER

namespace base {

// Get a thread-local object typed T. The object will be default-constructed
// at the first call to this function, and will be deleted when thread
// exits.
template <typename T> inline T* get_thread_local();

// |fn| or |fn(arg)| will be called at caller's exit. If caller is not a 
// thread, fn will be called at program termination. Calling sequence is LIFO:
// last registered function will be called first. Duplication of functions 
// are not checked. This function is often used for releasing thread-local
// resources declared with __thread (or thread_local defined in 
// base/thread_local.h) which is much faster than pthread_getspecific or
// boost::thread_specific_ptr.
// Returns 0 on success, -1 otherwise and errno is set.
int thread_atexit(void (*fn)());
int thread_atexit(void (*fn)(void*), void* arg);

// Remove registered function, matched functions will not be called.
void thread_atexit_cancel(void (*fn)());
void thread_atexit_cancel(void (*fn)(void*), void* arg);

// Delete the typed-T object whose address is `arg'. This is a common function
// to thread_atexit.
template <typename T> void delete_object(void* arg) {
    delete static_cast<T*>(arg);
}

}  // namespace base

#include "thread_local_inl.h"

#endif  // BRPC_BASE_THREAD_LOCAL_H
