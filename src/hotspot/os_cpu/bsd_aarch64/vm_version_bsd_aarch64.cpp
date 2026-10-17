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

#elif defined(__FreeBSD__) || defined(__OpenBSD__)

#include <machine/armreg.h>
#include <sys/auxv.h>

#define CPU_IMPL(midr)  (((midr) >> 24) & 0xff)
#define CPU_PART(midr)  (((midr) >> 4) & 0xfff)
#define CPU_VAR(midr)   (((midr) >> 20) & 0xf)
#define CPU_REV(midr)   (((midr) >> 0) & 0xf)

// FreeBSD runs every thread at the kernel's largest SVE vector length --
// sve_init() in sys/arm64/arm64/vfp.c sets ZCR_EL1 to ZCR_LEN_MASK -- and
// offers no way to shorten it, so answer with the length RDVL reads.
// Claiming the 16-byte minimum left C2's ptrue-governed code working on
// lanes it did not know were there.  These are only reached once HWCAP_SVE
// is set; OpenBSD gives userland no SVE, and keeps the minimum.
int VM_Version::get_current_sve_vector_length() {
#ifdef __FreeBSD__
  uint64_t vl;
  __asm__ volatile(".arch_extension sve\n\trdvl %0, #1\n\t.arch_extension nosve"
                   : "=r"(vl));
  return (int)vl;
#else
  return FloatRegister::sve_vl_min;
#endif
}

// The length cannot be changed; report the one in force.
int VM_Version::set_and_get_current_sve_vector_length(int length) {
  return get_current_sve_vector_length();
}

#ifdef __OpenBSD__
// For older processors on OpenBSD READ_SPECIALREG is not supported.
// These constants and tables allow for looking up the cpu and model.

#include <sys/types.h>
#include <sys/sysctl.h>
#include <string.h>
#include <stdio.h>

#define	CPU_IMPL_ARM		0x41
#define	CPU_IMPL_BROADCOM	0x42
#define	CPU_IMPL_CAVIUM		0x43
#define	CPU_IMPL_DEC		0x44
#define	CPU_IMPL_FUJITSU	0x46
#define	CPU_IMPL_HISILICON	0x48
#define	CPU_IMPL_INFINEON	0x49
#define	CPU_IMPL_FREESCALE	0x4D
#define	CPU_IMPL_NVIDIA		0x4E
#define	CPU_IMPL_APM		0x50
#define	CPU_IMPL_QUALCOMM	0x51
#define	CPU_IMPL_MARVELL	0x56
#define	CPU_IMPL_APPLE		0x61
#define	CPU_IMPL_INTEL		0x69
#define	CPU_IMPL_AMPERE		0xC0
#define	CPU_IMPL_MICROSOFT	0x6D

/* ARM Part numbers */
#define	CPU_PART_FOUNDATION	0xD00
#define	CPU_PART_CORTEX_A34	0xD02
#define	CPU_PART_CORTEX_A53	0xD03
#define	CPU_PART_CORTEX_A35	0xD04
#define	CPU_PART_CORTEX_A55	0xD05
#define	CPU_PART_CORTEX_A65	0xD06
#define	CPU_PART_CORTEX_A57	0xD07
#define	CPU_PART_CORTEX_A72	0xD08
#define	CPU_PART_CORTEX_A73	0xD09
#define	CPU_PART_CORTEX_A75	0xD0A
#define	CPU_PART_CORTEX_A76	0xD0B
#define	CPU_PART_NEOVERSE_N1	0xD0C
#define	CPU_PART_CORTEX_A77	0xD0D
#define	CPU_PART_CORTEX_A76AE	0xD0E
#define	CPU_PART_AEM_V8		0xD0F
#define	CPU_PART_NEOVERSE_V1	0xD40
#define	CPU_PART_CORTEX_A78	0xD41
#define	CPU_PART_CORTEX_A78AE	0xD42
#define	CPU_PART_CORTEX_A65AE	0xD43
#define	CPU_PART_CORTEX_X1	0xD44
#define	CPU_PART_CORTEX_A510	0xD46
#define	CPU_PART_CORTEX_A710	0xD47
#define	CPU_PART_CORTEX_X2	0xD48
#define	CPU_PART_NEOVERSE_N2	0xD49
#define	CPU_PART_NEOVERSE_E1	0xD4A
#define	CPU_PART_CORTEX_A78C	0xD4B
#define	CPU_PART_CORTEX_X1C	0xD4C
#define	CPU_PART_CORTEX_A715	0xD4D
#define	CPU_PART_CORTEX_X3	0xD4E
#define	CPU_PART_NEOVERSE_V2	0xD4F
#define	CPU_PART_CORTEX_A520	0xD80
#define	CPU_PART_CORTEX_A720	0xD81
#define	CPU_PART_CORTEX_X4	0xD82
#define	CPU_PART_NEOVERSE_V3AE	0xD83
#define	CPU_PART_NEOVERSE_V3	0xD84
#define	CPU_PART_CORTEX_X925	0xD85
#define	CPU_PART_CORTEX_A725	0xD87
#define	CPU_PART_C1_NANO	0xD8A
#define	CPU_PART_C1_PRO		0xD8B
#define	CPU_PART_C1_ULTRA	0xD8C
#define	CPU_PART_NEOVERSE_N3	0xD8E
#define	CPU_PART_C1_PREMIUM	0xD90

/* Cavium Part numbers */
#define	CPU_PART_THUNDERX	0x0A1
#define	CPU_PART_THUNDERX_81XX	0x0A2
#define	CPU_PART_THUNDERX_83XX	0x0A3
#define	CPU_PART_THUNDERX2	0x0AF

#define	CPU_REV_THUNDERX_1_0	0x00
#define	CPU_REV_THUNDERX_1_1	0x01

#define	CPU_REV_THUNDERX2_0	0x00

/* APM (now Ampere) Part number */
#define CPU_PART_EMAG8180	0x000

/* Ampere Part numbers */
#define	CPU_PART_AMPERE1	0xAC3
#define	CPU_PART_AMPERE1A	0xAC4

/* Microsoft Part numbers */
#define	CPU_PART_AZURE_COBALT_100	0xD49

/* Qualcomm */
#define CPU_PART_ORYON          0x001
#define	CPU_PART_KRYO400_GOLD	0x804
#define	CPU_PART_KRYO400_SILVER	0x805

/* Apple part numbers */
#define CPU_PART_M1_ICESTORM      0x022
#define CPU_PART_M1_FIRESTORM     0x023
#define CPU_PART_M1_ICESTORM_PRO  0x024
#define CPU_PART_M1_FIRESTORM_PRO 0x025
#define CPU_PART_M1_ICESTORM_MAX  0x028
#define CPU_PART_M1_FIRESTORM_MAX 0x029
#define CPU_PART_M2_BLIZZARD      0x032
#define CPU_PART_M2_AVALANCHE     0x033
#define CPU_PART_M2_BLIZZARD_PRO  0x034
#define CPU_PART_M2_AVALANCHE_PRO 0x035
#define CPU_PART_M2_BLIZZARD_MAX  0x038
#define CPU_PART_M2_AVALANCHE_MAX 0x039

struct cpu_parts {
	u_int		part_id;
	const char	*part_name;
};
#define	CPU_PART_NONE	{ 0, "Unknown Processor" }

struct cpu_implementers {
	u_int			impl_id;
	const char		*impl_name;
	/*
	 * Part number is implementation defined
	 * so each vendor will have its own set of values and names.
	 */
	const struct cpu_parts	*cpu_parts;
};
#define	CPU_IMPLEMENTER_NONE	{ 0, "Unknown Implementer", cpu_parts_none }

/*
 * Per-implementer table of (PartNum, CPU Name) pairs.
 */
/* ARM Ltd. From FreeBSD but using OpenBSD strings */
static const struct cpu_parts cpu_parts_arm[] = {
	{ CPU_PART_AEM_V8, "AEMv8" },
	{ CPU_PART_FOUNDATION, "Foundation-Model" },
	{ CPU_PART_CORTEX_A34, "Cortex-A34" },
	{ CPU_PART_CORTEX_A35, "Cortex-A35" },
	{ CPU_PART_CORTEX_A53, "Cortex-A53" },
	{ CPU_PART_CORTEX_A55, "Cortex-A55" },
	{ CPU_PART_CORTEX_A57, "Cortex-A57" },
	{ CPU_PART_CORTEX_A65, "Cortex-A65" },
	{ CPU_PART_CORTEX_A65AE, "Cortex-A65AE" },
	{ CPU_PART_CORTEX_A72, "Cortex-A72" },
	{ CPU_PART_CORTEX_A73, "Cortex-A73" },
	{ CPU_PART_CORTEX_A75, "Cortex-A75" },
	{ CPU_PART_CORTEX_A76, "Cortex-A76" },
	{ CPU_PART_CORTEX_A76AE, "Cortex-A76AE" },
	{ CPU_PART_CORTEX_A77, "Cortex-A77" },
	{ CPU_PART_CORTEX_A78, "Cortex-A78" },
	{ CPU_PART_CORTEX_A78AE, "Cortex-A78AE" },
	{ CPU_PART_CORTEX_A78C, "Cortex-A78C" },
	{ CPU_PART_CORTEX_A510, "Cortex-A510" },
	{ CPU_PART_CORTEX_A520, "Cortex-A520" },
	{ CPU_PART_CORTEX_A710, "Cortex-A710" },
	{ CPU_PART_CORTEX_A715, "Cortex-A715" },
	{ CPU_PART_CORTEX_A720, "Cortex-A720" },
	{ CPU_PART_CORTEX_A725, "Cortex-A725" },
	{ CPU_PART_CORTEX_X925, "Cortex-A925" },
	{ CPU_PART_CORTEX_X1C, "Cortex-X1C" },
	{ CPU_PART_CORTEX_X1, "Cortex-X1" },
	{ CPU_PART_CORTEX_X2, "Cortex-X2" },
	{ CPU_PART_CORTEX_X3, "Cortex-X3" },
	{ CPU_PART_CORTEX_X4, "Cortex-X4" },
	{ CPU_PART_C1_NANO, "C1-Nano" },
	{ CPU_PART_C1_PRO, "C1-Pro" },
	{ CPU_PART_C1_PREMIUM, "C1-Premium" },
	{ CPU_PART_C1_ULTRA, "C1-Ultra" },
	{ CPU_PART_NEOVERSE_E1, "Neoverse E1" },
	{ CPU_PART_NEOVERSE_N1, "Neoverse N1" },
	{ CPU_PART_NEOVERSE_N2, "Neoverse N2" },
	{ CPU_PART_NEOVERSE_N3, "Neoverse N3" },
	{ CPU_PART_NEOVERSE_V1, "Neoverse V1" },
	{ CPU_PART_NEOVERSE_V2, "Neoverse V2" },
	{ CPU_PART_NEOVERSE_V3, "Neoverse V3" },
	{ CPU_PART_NEOVERSE_V3AE, "Neoverse V3AE" },
	CPU_PART_NONE,
};

/* Cavium  From FreeBSD but using OpenBSD strings */
static const struct cpu_parts cpu_parts_cavium[] = {
	{ CPU_PART_THUNDERX, "ThunderX T88" },
	{ CPU_PART_THUNDERX_81XX, "ThunderX T81" },
	{ CPU_PART_THUNDERX_83XX, "ThunderX T83" },
	{ CPU_PART_THUNDERX2, "ThunderX2 T99" },
	CPU_PART_NONE,
};

/* APM (now Ampere), From FreeBSD but using OpenBSD strings */
static const struct cpu_parts cpu_parts_apm[] = {
	{ CPU_PART_EMAG8180, "X-Gene" },
	CPU_PART_NONE,
};

/* Ampere From FreeBSD, but using OpenBSD strings */
static const struct cpu_parts cpu_parts_ampere[] = {
	{ CPU_PART_AMPERE1A, "AmpereOne AC04" },
	{ CPU_PART_AMPERE1, "AmpereOne" },
	CPU_PART_NONE,
};

/* Microsoft */
static const struct cpu_parts cpu_parts_microsoft[] = {
	{ CPU_PART_AZURE_COBALT_100, "Azure Cobalt 100" },
	CPU_PART_NONE,
};

/* Qualcomm From FreeBSD & OpenBSD. */
static const struct cpu_parts cpu_parts_qcom[] = {
	{ CPU_PART_KRYO400_GOLD, "Kryo 400 Gold" },
	{ CPU_PART_KRYO400_SILVER, "Kryo 400 Silver" },
	{ CPU_PART_ORYON, "Oryon" },
	CPU_PART_NONE,
};

/* Apple. From FreeBSD but using OpenBSD strings */
static const struct cpu_parts cpu_parts_apple[] = {
	{ CPU_PART_M1_ICESTORM, "Icestorm" },
	{ CPU_PART_M1_FIRESTORM, "Firestorm" },
	{ CPU_PART_M1_ICESTORM_PRO, "Icestorm Pro" },
	{ CPU_PART_M1_FIRESTORM_PRO, "Firestorm Pro" },
	{ CPU_PART_M1_ICESTORM_MAX, "Icestorm Max" },
	{ CPU_PART_M1_FIRESTORM_MAX, "Firestorm Max" },
	{ CPU_PART_M2_BLIZZARD, "Blizzard" },
	{ CPU_PART_M2_AVALANCHE, "Avalanche" },
	{ CPU_PART_M2_BLIZZARD_PRO, "Blizzard Pro" },
	{ CPU_PART_M2_AVALANCHE_PRO, "Avalanche Pro" },
	{ CPU_PART_M2_BLIZZARD_MAX, "Blizzard Max" },
	{ CPU_PART_M2_AVALANCHE_MAX, "Avalanche Max" },
	CPU_PART_NONE,
};

/* Unknown */
static const struct cpu_parts cpu_parts_none[] = {
	CPU_PART_NONE,
};

/*
 * Implementers table. From FreeBSD, but using OpenBSD strings
 */
const struct cpu_implementers cpu_implementers[] = {
	{ CPU_IMPL_AMPERE,	"Ampere",	cpu_parts_ampere },
	{ CPU_IMPL_APPLE,	"Apple",	cpu_parts_apple },
	{ CPU_IMPL_APM,		"Applied Micro",cpu_parts_apm },
	{ CPU_IMPL_ARM,		"ARM",		cpu_parts_arm },
	{ CPU_IMPL_BROADCOM,	"Broadcom",	cpu_parts_none },
	{ CPU_IMPL_CAVIUM,	"Cavium",	cpu_parts_cavium },
	{ CPU_IMPL_DEC,		"DEC",		cpu_parts_none },
	{ CPU_IMPL_FREESCALE,	"Freescale",	cpu_parts_none },
	{ CPU_IMPL_FUJITSU,	"Fujitsu",	cpu_parts_none },
	{ CPU_IMPL_HISILICON,	"HiSilicon",	cpu_parts_none },
	{ CPU_IMPL_INFINEON,	"IFX",		cpu_parts_none },
	{ CPU_IMPL_INTEL,	"Intel",	cpu_parts_none },
	{ CPU_IMPL_MARVELL,	"Marvell",	cpu_parts_none },
	{ CPU_IMPL_MICROSOFT,	"Microsoft",	cpu_parts_microsoft },
	{ CPU_IMPL_NVIDIA,	"NVIDIA",	cpu_parts_none },
	{ CPU_IMPL_QUALCOMM,	"Qualcomm",	cpu_parts_qcom },
	CPU_IMPLEMENTER_NONE,
};

static void
lookup_cpu(int &_cpu, int &_model, int &_variant, int &_revision) {
  int mib[] = { CTL_HW, HW_MODEL };
  char descr[BUFSIZ];
  char *part_name, *rv_str;
  size_t descr_len = sizeof(descr);
  size_t impl_name_len;
  const struct cpu_parts *cpu_partsp = nullptr;
  size_t i;

  if (sysctl(mib, ARRAY_SIZE(mib), &descr, &descr_len, nullptr, 0) == -1)
    return;

  for (i = 0; i < ARRAY_SIZE(cpu_implementers); i++) {
    impl_name_len = strlen(cpu_implementers[i].impl_name);
    if (cpu_implementers[i].impl_id == 0 ||
      strncmp(descr, cpu_implementers[i].impl_name, impl_name_len) == 0) {
      _cpu = cpu_implementers[i].impl_id;
      cpu_partsp = cpu_implementers[i].cpu_parts;
      break;
    }
  }

  if (_cpu == 0)
    return;

  // +1 to skip space +1 more because descr_len includes NUL
  if (impl_name_len + 2 > descr_len)
    return;

  part_name = &descr[impl_name_len+1];

  rv_str = strrchr(part_name, ' ');
  if (rv_str == nullptr)
    return;

  // null term part_name and skip over it
  *(rv_str++) = '\0';

  for (i = 0; ; i++) {
    if (cpu_partsp[i].part_id == 0 ||
      strcmp(part_name, cpu_partsp[i].part_name) == 0) {
      _model = cpu_partsp[i].part_id;
      break;
    }
  }

  sscanf(rv_str, "r%up%u", &_variant, &_revision);
}
#endif // __OpenBSD__

static uint64_t check_feature(unsigned long hwcap, VM_Version::Feature_Flag flag,
                              unsigned long hwcap_bitmask) {
  return (hwcap & hwcap_bitmask) != 0 ? BIT_MASK(flag) : 0;
}

void VM_Version::get_os_cpu_info() {
  // FreeBSD and OpenBSD report the features through the aux vector, with
  // the same HWCAP and HWCAP2 bits as Linux.  Collect the same set the
  // Linux reader does.  Some CPUs (Apple's M4) implement SME but not SVE
  // and still report SVE2, so the SVE2 features are only taken along
  // with SVE.
  unsigned long auxv = 0;
  unsigned long auxv2 = 0;
  elf_aux_info(AT_HWCAP, &auxv, sizeof(auxv));
  elf_aux_info(AT_HWCAP2, &auxv2, sizeof(auxv2));

  _features =
      check_feature(auxv,  CPU_FP,      HWCAP_FP) |
      check_feature(auxv,  CPU_ASIMD,   HWCAP_ASIMD) |
      check_feature(auxv,  CPU_EVTSTRM, HWCAP_EVTSTRM) |
      check_feature(auxv,  CPU_AES,     HWCAP_AES) |
      check_feature(auxv,  CPU_PMULL,   HWCAP_PMULL) |
      check_feature(auxv,  CPU_SHA1,    HWCAP_SHA1) |
      check_feature(auxv,  CPU_SHA2,    HWCAP_SHA2) |
      check_feature(auxv,  CPU_CRC32,   HWCAP_CRC32) |
      check_feature(auxv,  CPU_LSE,     HWCAP_ATOMICS) |
      check_feature(auxv,  CPU_DCPOP,   HWCAP_DCPOP) |
      check_feature(auxv,  CPU_SHA3,    HWCAP_SHA3) |
      check_feature(auxv,  CPU_SHA512,  HWCAP_SHA512) |
      check_feature(auxv,  CPU_SVE,     HWCAP_SVE) |
      check_feature(auxv,  CPU_PACA,    HWCAP_PACA) |
      check_feature(auxv,  CPU_FPHP,    HWCAP_FPHP) |
      check_feature(auxv,  CPU_ASIMDHP, HWCAP_ASIMDHP);
#ifdef HWCAP2_ECV
  _features |= check_feature(auxv2, CPU_ECV, HWCAP2_ECV);
#endif
#ifdef HWCAP2_WFXT
  _features |= check_feature(auxv2, CPU_WFXT, HWCAP2_WFXT);
#endif
  if (auxv & HWCAP_SVE) {
    _features |= check_feature(auxv2, CPU_SVE2,       HWCAP2_SVE2) |
                 check_feature(auxv2, CPU_SVEBITPERM, HWCAP2_SVEBITPERM);
  }

#ifdef __FreeBSD__
  // FreeBSD's fixed length can be anything up to 256 bytes, the largest SVE
  // allows and what QEMU's emulated CPU offers.  C2 at that length gets
  // wrong vector results -- TestStringCompareToSameLength, the superword
  // TestAlignVectorFuzzer and Test7196199 in tier1 -- where Linux, whose
  // threads start at 64 bytes, is not run.  Keep SVE to the lengths a VM
  // is used at elsewhere, and use NEON beyond them.
  if ((auxv & HWCAP_SVE) && get_current_sve_vector_length() > 64) {
    clear_feature(CPU_SVE);
    clear_feature(CPU_SVE2);
    clear_feature(CPU_SVEBITPERM);
  }
#endif

  // FreeBSD lets EL0 read MIDR_EL1.  OpenBSD does so only when it sets
  // HWCAP_CPUID, and otherwise names the CPU in sysctl hw.model.
#ifdef __FreeBSD__
  uint32_t midr = READ_SPECIALREG(midr_el1);
  _cpu = CPU_IMPL(midr);
  _model = CPU_PART(midr);
  _variant = CPU_VAR(midr);
  _revision = CPU_REV(midr);
#else
  if (auxv & HWCAP_CPUID) {
    uint32_t midr = READ_SPECIALREG(midr_el1);
    _cpu = CPU_IMPL(midr);
    _model = CPU_PART(midr);
    _variant = CPU_VAR(midr);
    _revision = CPU_REV(midr);
  } else {
    lookup_cpu(_cpu, _model, _variant, _revision);
  }
#endif

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
