/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2026 SAP SE. All rights reserved.
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

#include "runtime/vm_version.hpp"

#include <sys/types.h>
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif

// The BSDs have no _SC_LEVEL1_DCACHE_LINESIZE or _SC_LEVEL1_ICACHE_LINESIZE.
// FreeBSD reports the line size its kernel flushes with; anywhere else, or
// should that fail, use DEFAULT_CACHE_LINE_SIZE, which is the correct value
// for all currently supported processors.
static int cache_line_size() {
#ifdef __FreeBSD__
  int size = 0;
  size_t len = sizeof(size);
  if (sysctlbyname("machdep.cacheline_size", &size, &len, nullptr, 0) == 0 && size > 0) {
    return size;
  }
#endif
  return DEFAULT_CACHE_LINE_SIZE;
}

int VM_Version::get_dcache_line_size() {
  return cache_line_size();
}

int VM_Version::get_icache_line_size() {
  return cache_line_size();
}
