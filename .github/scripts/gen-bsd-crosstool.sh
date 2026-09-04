#!/bin/bash
# Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
# DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
#
# This code is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License version 2 only, as
# published by the Free Software Foundation.  Oracle designates this
# particular file as subject to the "Classpath" exception as provided
# by Oracle in the LICENSE file that accompanied this code.
#
# This code is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# version 2 for more details (a copy is included in the LICENSE file that
# accompanied this code).
#
# You should have received a copy of the GNU General Public License version
# 2 along with this work; if not, write to the Free Software Foundation,
# Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
#
# Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
# or visit www.oracle.com if you need additional information or have any
# questions.

# Writes a set of <triple>-clang wrappers that drive the host clang at a BSD
# sysroot, plus the llvm binutils under the names the build looks for.  A
# wrapper rather than a set of flags because configure records the compiler
# as one word and passes it around that way.
#
# Usage: gen-bsd-crosstool.sh <os> <triple> <sysroot> <bindir>

set -eu

os="$1"
triple="$2"
sysroot="$3"
bindir="$4"
mkdir -p "$bindir"

fixups="$bindir/bsd-clang-fixups.h"
cat > "$fixups" <<'H'
/* The BSD headers ask for these gcc-internal macros; clang defines neither
   the minima nor the sig_atomic_t type, only the maxima. */
#ifndef __WCHAR_MIN__
#define __WCHAR_MIN__ (-__WCHAR_MAX__ - 1)
#endif
#ifndef __WINT_MIN__
#define __WINT_MIN__ 0U
#endif
#ifndef __SIG_ATOMIC_TYPE__
#define __SIG_ATOMIC_TYPE__ int
#endif
#ifndef __SIG_ATOMIC_MIN__
#define __SIG_ATOMIC_MIN__ (-__SIG_ATOMIC_MAX__ - 1)
#endif
H

# NetBSD and DragonFly ship libstdc++ and keep its headers under
# /usr/include/g++, which clang does not look in even when told to target
# them.  FreeBSD and OpenBSD ship libc++, which clang finds by itself.
case "$os" in
  netbsd|dragonfly)
    cxx_extra="-stdlib=libstdc++ -isystem $sysroot/usr/include/g++"
    rt_extra="--rtlib=libgcc"
    link_extra="-lgcc"
    ;;
  *)
    cxx_extra=""
    rt_extra=""
    link_extra=""
    ;;
esac

for tool in clang clang++; do
  case "$tool" in
    clang++) extra="$cxx_extra" ;;
    *)       extra="" ;;
  esac
  cat > "$bindir/$triple-$tool" <<W
#!/bin/sh
exec /usr/bin/$tool --target=$triple --sysroot=$sysroot \\
  $rt_extra -fuse-ld=lld $extra \\
  -include $fixups "\$@" $link_extra
W
  chmod +x "$bindir/$triple-$tool"
done

for tool in ar ranlib strip objcopy nm objdump; do
  ln -sf "/usr/bin/llvm-$tool" "$bindir/$triple-$tool"
done

# Prove the wrapper links before configure spends ten minutes finding out.
tmp=$(mktemp -d)
printf 'int main(void){return 0;}\n' > "$tmp/probe.c"
"$bindir/$triple-clang" "$tmp/probe.c" -o "$tmp/probe"
file "$tmp/probe"
rm -rf "$tmp"
