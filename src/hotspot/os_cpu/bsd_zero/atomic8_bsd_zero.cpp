/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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

// 32-bit PowerPC has no 8-byte atomic instructions, so clang compiles the
// 8-byte operations in atomicAccess_bsd_zero.hpp into calls to
// __atomic_load_8 and its siblings, the way gcc does everywhere such a
// machine lacks them.  On Linux, libatomic answers those calls, and
// libraries.m4 links it for every 32-bit Zero.  No BSD has a libatomic in
// its base system, and neither the C library nor compiler-rt defines the
// calls, so libjvm carries them itself: the same striped spin locks
// libatomic falls back on, taken on a 4-byte word the machine can swap
// atomically.  Every 8-byte atomic in libjvm comes here, loads and stores
// included, so they all agree on the lock for an address.  libjvm is
// built with hidden visibility, so none of this escapes it.

#if defined(__GCC_ATOMIC_LLONG_LOCK_FREE) && __GCC_ATOMIC_LLONG_LOCK_FREE < 2

#include <stdint.h>

namespace {

const int bsd_zero_lock_count = 64;
volatile int bsd_zero_locks[bsd_zero_lock_count];

class BsdZeroLock {
  volatile int* _lock;
 public:
  BsdZeroLock(const volatile void* p)
    : _lock(&bsd_zero_locks[(reinterpret_cast<uintptr_t>(p) >> 3) % bsd_zero_lock_count]) {
    while (__atomic_exchange_n(_lock, 1, __ATOMIC_ACQUIRE) != 0) {
      while (__atomic_load_n(_lock, __ATOMIC_RELAXED) != 0) { }
    }
    // Callers may ask for sequential consistency; the lock alone gives
    // acquire and release.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
  }
  ~BsdZeroLock() {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(_lock, 0, __ATOMIC_RELEASE);
  }
};

} // namespace

// clang will not have a builtin's name defined in C or C++, so each is
// defined under a name of its own and given the builtin's in the object.
#define BSD_ZERO_ATOMIC8(ret, name, params) \
  extern "C" ret bsd_zero_##name params __asm__("__" #name); \
  extern "C" ret bsd_zero_##name params

BSD_ZERO_ATOMIC8(uint64_t, atomic_load_8, (const volatile void* mem, int model)) {
  BsdZeroLock lock(mem);
  return *static_cast<const volatile uint64_t*>(mem);
}

BSD_ZERO_ATOMIC8(void, atomic_store_8, (volatile void* mem, uint64_t val, int model)) {
  BsdZeroLock lock(mem);
  *static_cast<volatile uint64_t*>(mem) = val;
}

BSD_ZERO_ATOMIC8(uint64_t, atomic_exchange_8, (volatile void* mem, uint64_t val, int model)) {
  BsdZeroLock lock(mem);
  volatile uint64_t* p = static_cast<volatile uint64_t*>(mem);
  uint64_t old = *p;
  *p = val;
  return old;
}

BSD_ZERO_ATOMIC8(bool, atomic_compare_exchange_8,
                 (volatile void* mem, void* expected, uint64_t desired,
                  int success, int failure)) {
  BsdZeroLock lock(mem);
  volatile uint64_t* p = static_cast<volatile uint64_t*>(mem);
  uint64_t* e = static_cast<uint64_t*>(expected);
  uint64_t old = *p;
  if (old == *e) {
    *p = desired;
    return true;
  }
  *e = old;
  return false;
}

#define BSD_ZERO_ATOMIC8_OP(op, expr)                                                  \
  BSD_ZERO_ATOMIC8(uint64_t, atomic_fetch_##op##_8,                                    \
                   (volatile void* mem, uint64_t val, int model)) {                    \
    BsdZeroLock lock(mem);                                                             \
    volatile uint64_t* p = static_cast<volatile uint64_t*>(mem);                       \
    uint64_t old = *p;                                                                 \
    *p = (expr);                                                                       \
    return old;                                                                        \
  }                                                                                    \
  BSD_ZERO_ATOMIC8(uint64_t, atomic_##op##_fetch_8,                                    \
                   (volatile void* mem, uint64_t val, int model)) {                    \
    BsdZeroLock lock(mem);                                                             \
    volatile uint64_t* p = static_cast<volatile uint64_t*>(mem);                       \
    uint64_t old = *p;                                                                 \
    *p = (expr);                                                                       \
    return *p;                                                                         \
  }

BSD_ZERO_ATOMIC8_OP(add, old + val)
BSD_ZERO_ATOMIC8_OP(sub, old - val)
BSD_ZERO_ATOMIC8_OP(and, old & val)
BSD_ZERO_ATOMIC8_OP(or,  old | val)
BSD_ZERO_ATOMIC8_OP(xor, old ^ val)

#endif
