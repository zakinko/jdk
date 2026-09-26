/*
 * Copyright (c) 2003, 2026, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2007, 2008, 2011, 2015, Red Hat, Inc.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef OS_CPU_BSD_ZERO_ATOMICACCESS_BSD_ZERO_HPP
#define OS_CPU_BSD_ZERO_ATOMICACCESS_BSD_ZERO_HPP

#include "orderAccess_bsd_zero.hpp"

// Implementation of class AtomicAccess

// The 8-byte operations below go through an 8-byte aligned view of their
// operand.  The i386 ABI aligns a 64-bit integer to four bytes, so without
// it clang cannot assume a naturally aligned operand and turns every one of
// them into a call to __atomic_*_8 -- which nothing on the NetBSD link line
// provides -- and warns that it did (-Watomic-alignment).  With it, clang
// emits lock cmpxchg8b, which is atomic at any alignment, as gcc does
// without being told.  Where the ABI already aligns them to eight, as on
// 32-bit arm and on every 64-bit machine, this changes nothing.
template<typename T>
struct BsdZeroAligned8 {
  typedef T type __attribute__((aligned(8)));
};

template<typename T>
inline typename BsdZeroAligned8<T>::type volatile* bsd_zero_aligned8(T volatile* p) {
  return reinterpret_cast<typename BsdZeroAligned8<T>::type volatile*>(p);
}

template<size_t byte_size>
struct AtomicAccess::PlatformAdd {
  template<typename D, typename I>
  D add_then_fetch(D volatile* dest, I add_value, atomic_memory_order order) const;

  template<typename D, typename I>
  D fetch_then_add(D volatile* dest, I add_value, atomic_memory_order order) const {
    return add_then_fetch(dest, add_value, order) - add_value;
  }
};

template<>
template<typename D, typename I>
inline D AtomicAccess::PlatformAdd<4>::add_then_fetch(D volatile* dest, I add_value,
                                                      atomic_memory_order order) const {
  STATIC_ASSERT(4 == sizeof(I));
  STATIC_ASSERT(4 == sizeof(D));

  D res = __atomic_add_fetch(dest, add_value, __ATOMIC_RELEASE);
  FULL_MEM_BARRIER;
  return res;
}

template<>
template<typename D, typename I>
inline D AtomicAccess::PlatformAdd<8>::add_then_fetch(D volatile* dest, I add_value,
                                                      atomic_memory_order order) const {
  STATIC_ASSERT(8 == sizeof(I));
  STATIC_ASSERT(8 == sizeof(D));

  D res = __atomic_add_fetch(bsd_zero_aligned8(dest), add_value, __ATOMIC_RELEASE);
  FULL_MEM_BARRIER;
  return res;
}

template<>
struct AtomicAccess::PlatformXchg<1> : AtomicAccess::XchgUsingCmpxchg<1> {};

template<>
template<typename T>
inline T AtomicAccess::PlatformXchg<4>::operator()(T volatile* dest,
                                                   T exchange_value,
                                                   atomic_memory_order order) const {
  STATIC_ASSERT(4 == sizeof(T));
  FULL_MEM_BARRIER;
  T result = __atomic_exchange_n(dest, exchange_value, __ATOMIC_RELAXED);
  FULL_MEM_BARRIER;
  return result;
}

template<>
template<typename T>
inline T AtomicAccess::PlatformXchg<8>::operator()(T volatile* dest,
                                                   T exchange_value,
                                                   atomic_memory_order order) const {
  STATIC_ASSERT(8 == sizeof(T));
  FULL_MEM_BARRIER;
  T result = __atomic_exchange_n(bsd_zero_aligned8(dest), exchange_value, __ATOMIC_RELAXED);
  FULL_MEM_BARRIER;
  return result;
}

// No direct support for cmpxchg of bytes; emulate using int.
template<>
struct AtomicAccess::PlatformCmpxchg<1> : AtomicAccess::CmpxchgByteUsingInt {};

template<>
template<typename T>
inline T AtomicAccess::PlatformCmpxchg<4>::operator()(T volatile* dest,
                                                      T compare_value,
                                                      T exchange_value,
                                                      atomic_memory_order order) const {
  STATIC_ASSERT(4 == sizeof(T));
  T value = compare_value;
  FULL_MEM_BARRIER;
  __atomic_compare_exchange(dest, &value, &exchange_value, /*weak*/false,
                            __ATOMIC_RELAXED, __ATOMIC_RELAXED);
  FULL_MEM_BARRIER;
  return value;
}

template<>
template<typename T>
inline T AtomicAccess::PlatformCmpxchg<8>::operator()(T volatile* dest,
                                                      T compare_value,
                                                      T exchange_value,
                                                      atomic_memory_order order) const {
  STATIC_ASSERT(8 == sizeof(T));

  T value = compare_value;
  FULL_MEM_BARRIER;
  __atomic_compare_exchange(bsd_zero_aligned8(dest), &value, &exchange_value, /*weak*/false,
                            __ATOMIC_RELAXED, __ATOMIC_RELAXED);
  FULL_MEM_BARRIER;
  return value;
}

// Atomically copy 64 bits of data
inline void atomic_copy64(const volatile void *src, volatile void *dst) {
  int64_t tmp;
  __atomic_load(bsd_zero_aligned8(reinterpret_cast<volatile int64_t*>(const_cast<volatile void*>(src))),
                &tmp, __ATOMIC_RELAXED);
  __atomic_store(bsd_zero_aligned8(reinterpret_cast<volatile int64_t*>(dst)), &tmp, __ATOMIC_RELAXED);
}

template<>
template<typename T>
inline T AtomicAccess::PlatformLoad<8>::operator()(T const volatile* src) const {
  STATIC_ASSERT(8 == sizeof(T));
  T dest;
  __atomic_load(bsd_zero_aligned8(const_cast<T*>(src)), &dest, __ATOMIC_RELAXED);
  return dest;
}

template<>
template<typename T>
inline void AtomicAccess::PlatformStore<8>::operator()(T volatile* dest,
                                                       T store_value) const {
  STATIC_ASSERT(8 == sizeof(T));
  __atomic_store(bsd_zero_aligned8(dest), &store_value, __ATOMIC_RELAXED);
}

#endif // OS_CPU_BSD_ZERO_ATOMICACCESS_BSD_ZERO_HPP
