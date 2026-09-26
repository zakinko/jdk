/*
 * Copyright (c) 2006, 2025, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2014, 2019, Red Hat Inc. All rights reserved.
 * Copyright (c) 2021, Azul Systems, Inc. All rights reserved.
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

#include "register_aarch64.hpp"
#include "runtime/java.hpp"
#include "runtime/os.hpp"
#include "runtime/vm_version.hpp"

#include <sys/sysctl.h>

#ifdef __APPLE__

int VM_Version::get_current_sve_vector_length() {
  ShouldNotCallThis();
  return -1;
}

int VM_Version::set_and_get_current_sve_vector_length(int length) {
  ShouldNotCallThis();
  return -1;
}

static bool cpu_has(const char* optional) {
  uint32_t val;
  size_t len = sizeof(val);
  if (sysctlbyname(optional, &val, &len, nullptr, 0)) {
    return false;
  }
  return val;
}

void VM_Version::get_os_cpu_info() {
  size_t sysctllen;

  // cpu_has() uses sysctlbyname function to check the existence of CPU
  // features. References: Apple developer document [1] and XNU kernel [2].
  // [1] https://developer.apple.com/documentation/kernel/1387446-sysctlbyname/determining_instruction_set_characteristics
  // [2] https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_mib.c
  //
  // Note that for some features (e.g., LSE, SHA512 and SHA3) there are two
  // parameters for sysctlbyname, which are invented at different times.
  // Considering backward compatibility, we check both here.
  //
  // Floating-point and Advance SIMD features are standard in Apple processors
  // beginning with M1 and A7, and don't need to be checked [1].
  // 1) hw.optional.floatingpoint always returns 1 [2].
  // 2) ID_AA64PFR0_EL1 describes AdvSIMD always equals to FP field.
  //    See the Arm ARM, section "ID_AA64PFR0_EL1, AArch64 Processor Feature
  //    Register 0".
  set_feature(CPU_FP);
  set_feature(CPU_ASIMD);

  // All Apple-darwin Arm processors have AES, PMULL, SHA1 and SHA2.
  // See https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/arm/commpage/commpage.c#L412
  // Note that we ought to add assertions to check sysctlbyname parameters for
  // these four CPU features, e.g., "hw.optional.arm.FEAT_AES", but the
  // corresponding string names are not available before xnu-8019 version.
  // Hence, assertions are omitted considering backward compatibility.
  set_feature(CPU_AES);
  set_feature(CPU_PMULL);
  set_feature(CPU_SHA1);
  set_feature(CPU_SHA2);

  if (cpu_has("hw.optional.armv8_crc32")) {
    set_feature(CPU_CRC32);
  }
  if (cpu_has("hw.optional.arm.FEAT_LSE") ||
      cpu_has("hw.optional.armv8_1_atomics")) {
    set_feature(CPU_LSE);
  }
  if (cpu_has("hw.optional.arm.FEAT_SHA512") ||
      cpu_has("hw.optional.armv8_2_sha512")) {
    set_feature(CPU_SHA512);
  }
  if (cpu_has("hw.optional.arm.FEAT_SHA3") ||
      cpu_has("hw.optional.armv8_2_sha3")) {
    set_feature(CPU_SHA3);
  }
  if (cpu_has("hw.optional.arm.FEAT_SB")) {
    set_feature(CPU_SB);
  }

  int cache_line_size;
  int hw_conf_cache_line[] = { CTL_HW, HW_CACHELINE };
  sysctllen = sizeof(cache_line_size);
  if (sysctl(hw_conf_cache_line, 2, &cache_line_size, &sysctllen, nullptr, 0)) {
    cache_line_size = 16;
  }
  _icache_line_size = 16; // minimal line length CCSIDR_EL1 can hold
  _dcache_line_size = cache_line_size;

  uint64_t dczid_el0;
  __asm__ (
    "mrs %0, DCZID_EL0\n"
    : "=r"(dczid_el0)
  );
  if (!(dczid_el0 & 0x10)) {
    _zva_length = 4 << (dczid_el0 & 0xf);
  }

  int family;
  sysctllen = sizeof(family);
  if (sysctlbyname("hw.cpufamily", &family, &sysctllen, nullptr, 0)) {
    family = 0;
  }

  _model = family;
  _cpu = CPU_APPLE;
}

#elif defined(__NetBSD__)

#include <machine/armreg.h>

#define CPU_IMPL(midr)  (((midr) >> 24) & 0xff)
#define CPU_PART(midr)  (((midr) >> 4) & 0xfff)
#define CPU_VAR(midr)   (((midr) >> 20) & 0xf)
#define CPU_REV(midr)   (((midr) >> 0) & 0xf)

// NetBSD offers no call to ask for the SVE vector length, so answer with
// the architectural minimum.
int VM_Version::get_current_sve_vector_length() {
  return FloatRegister::sve_vl_min;
}

int VM_Version::set_and_get_current_sve_vector_length(int length) {
  return FloatRegister::sve_vl_min;
}

void VM_Version::get_os_cpu_info() {
  // NetBSD has neither <sys/auxv.h> nor AT_HWCAP, and traps an MRS of the
  // EL1 identification registers, but hands every CPU's copy of them over
  // through sysctl machdep.cpuN.cpu_id.  A feature is reported only when
  // every CPU has it, since a thread may run on any of them.
  size_t len;
  int mib[] = { CTL_HW, HW_NCPU };
  int ncpu;
  struct aarch64_sysctl_cpu_id id;
  char path[32];
  int num_fp = 0;
  int num_asimd = 0;
  int num_aes = 0;
  int num_pmull = 0;
  int num_sha1 = 0;
  int num_sha2 = 0;
  int num_crc32 = 0;
  int num_lse = 0;
  int num_dcpop = 0;
  int num_sha3 = 0;
  int num_sha512 = 0;
  int num_sve = 0;
  int num_paca = 0;
  len = sizeof(ncpu);
  if (sysctl(mib, 2, &ncpu, &len, nullptr, 0) < 0) {
    return;
  }
  for (int curcpu = 0; curcpu < ncpu; curcpu++) {
    len = sizeof(id);
    os::snprintf_checked(path, sizeof(path), "machdep.cpu%d.cpu_id", curcpu);
    if (sysctlbyname(path, &id, &len, nullptr, 0) < 0) {
      continue;
    }

    if (__SHIFTOUT(id.ac_aa64pfr0, ID_AA64PFR0_EL1_FP) == ID_AA64PFR0_EL1_FP_IMPL)
      num_fp++;
    if (__SHIFTOUT(id.ac_aa64pfr0, ID_AA64PFR0_EL1_ADVSIMD) == ID_AA64PFR0_EL1_ADV_SIMD_IMPL)
      num_asimd++;
    if (__SHIFTOUT(id.ac_aa64isar0, ID_AA64ISAR0_EL1_AES) >= ID_AA64ISAR0_EL1_AES_AES)
      num_aes++;
    if (__SHIFTOUT(id.ac_aa64isar0, ID_AA64ISAR0_EL1_AES) >= ID_AA64ISAR0_EL1_AES_PMUL)
      num_pmull++;
    if (__SHIFTOUT(id.ac_aa64isar0, ID_AA64ISAR0_EL1_SHA1) >= ID_AA64ISAR0_EL1_SHA1_SHA1CPMHSU)
      num_sha1++;
    if (__SHIFTOUT(id.ac_aa64isar0, ID_AA64ISAR0_EL1_SHA2) >= ID_AA64ISAR0_EL1_SHA2_SHA256HSU)
      num_sha2++;
    if (__SHIFTOUT(id.ac_aa64isar0, ID_AA64ISAR0_EL1_CRC32) >= ID_AA64ISAR0_EL1_CRC32_CRC32X)
      num_crc32++;
#if defined(ID_AA64ISAR0_EL1_ATOMIC_SWP)
    if (__SHIFTOUT(id.ac_aa64isar0, ID_AA64ISAR0_EL1_ATOMIC) >= ID_AA64ISAR0_EL1_ATOMIC_SWP)
      num_lse++;
#endif
#if defined(ID_AA64ISAR1_EL1_DPB_CVAP)
    if (__SHIFTOUT(id.ac_aa64isar1, ID_AA64ISAR1_EL1_DPB) >= ID_AA64ISAR1_EL1_DPB_CVAP)
      num_dcpop++;
#endif
#if defined(ID_AA64ISAR0_EL1_SHA3_EOR3)
    if (__SHIFTOUT(id.ac_aa64isar0, ID_AA64ISAR0_EL1_SHA3) >= ID_AA64ISAR0_EL1_SHA3_EOR3)
      num_sha3++;
#endif
#if defined(ID_AA64ISAR0_EL1_SHA2_SHA512HSU)
    if (__SHIFTOUT(id.ac_aa64isar0, ID_AA64ISAR0_EL1_SHA2) >= ID_AA64ISAR0_EL1_SHA2_SHA512HSU)
      num_sha512++;
#endif
#if defined(ID_AA64PFR0_EL1_SVE_IMPL)
    if (__SHIFTOUT(id.ac_aa64pfr0, ID_AA64PFR0_EL1_SVE) >= ID_AA64PFR0_EL1_SVE_IMPL)
      num_sve++;
#endif
#if defined(ID_AA64ISAR1_EL1_APA_QARMA) && defined(ID_AA64ISAR1_EL1_API_SUPPORTED)
    if (__SHIFTOUT(id.ac_aa64isar1, ID_AA64ISAR1_EL1_APA) >= ID_AA64ISAR1_EL1_APA_QARMA ||
        __SHIFTOUT(id.ac_aa64isar1, ID_AA64ISAR1_EL1_API) >= ID_AA64ISAR1_EL1_API_SUPPORTED)
      num_paca++;
#endif
  }
  if (num_fp == ncpu)     set_feature(CPU_FP);
  if (num_asimd == ncpu)  set_feature(CPU_ASIMD);
  if (num_aes == ncpu)    set_feature(CPU_AES);
  if (num_pmull == ncpu)  set_feature(CPU_PMULL);
  if (num_sha1 == ncpu)   set_feature(CPU_SHA1);
  if (num_sha2 == ncpu)   set_feature(CPU_SHA2);
  if (num_crc32 == ncpu)  set_feature(CPU_CRC32);
  if (num_lse == ncpu)    set_feature(CPU_LSE);
  if (num_dcpop == ncpu)  set_feature(CPU_DCPOP);
  if (num_sha3 == ncpu)   set_feature(CPU_SHA3);
  if (num_sha512 == ncpu) set_feature(CPU_SHA512);
  if (num_sve == ncpu)    set_feature(CPU_SVE);
  if (num_paca == ncpu)   set_feature(CPU_PACA);

  _cpu = CPU_IMPL(id.ac_midr);
  _model = CPU_PART(id.ac_midr);
  _variant = CPU_VAR(id.ac_midr);
  _revision = CPU_REV(id.ac_midr);

  // CTR_EL0 and DCZID_EL0 are readable at EL0, as on Linux.
  uint64_t ctr_el0;
  uint64_t dczid_el0;
  __asm__ (
    "mrs %0, CTR_EL0\n"
    "mrs %1, DCZID_EL0\n"
    : "=r"(ctr_el0), "=r"(dczid_el0)
  );

  _icache_line_size = (1 << (ctr_el0 & 0x0f)) * 4;
  _dcache_line_size = (1 << ((ctr_el0 >> 16) & 0x0f)) * 4;

  if (!(dczid_el0 & 0x10)) {
    _zva_length = 4 << (dczid_el0 & 0xf);
  }
}

#else
#error "no CPU feature reader for this BSD on aarch64"
#endif

void VM_Version::get_compatible_board(char *buf, int buflen) {
  assert(buf != nullptr, "invalid argument");
  assert(buflen >= 1, "invalid argument");
  *buf = '\0';
}

#ifdef __APPLE__

bool VM_Version::is_cpu_emulated() {
  return false;
}

#endif
