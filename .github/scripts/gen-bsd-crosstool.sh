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
# Usage: gen-bsd-crosstool.sh <os> <triple> <sysroot> <bindir> [suffix]
#
# The suffix picks a particular LLVM, as in "-20" for clang-20 and
# llvm-nm-20.  Some targets need one: clang 18 crashes in the post-RA
# pass on aarch64-unknown-openbsd.  Without it the unversioned tools
# are used.

set -eu

os="$1"
triple="$2"
sysroot="$3"
bindir="$4"
llvm_suffix="${5-}"
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

# GNU ld only knows the architecture it was built for, so take the one
# that matches the target rather than /usr/bin/ld.bfd, which is the
# host's and rejects an aarch64 crt0.o as "file in wrong format".
# Debian names the 32-bit x86 toolchain after i686, not after i386.
# 32-bit arm is the one whose tuple does not end in -gnu, and 64-bit mips
# is named after its ABI as well.
gnu_ld() {
  case "${triple%%-*}" in
    i386)  gnu=i686-linux-gnu ;;
    armv6|armv7) gnu=arm-linux-gnueabihf ;;
    mips64|mips64el) gnu=${triple%%-*}-linux-gnuabi64 ;;
    *)     gnu=${triple%%-*}-linux-gnu ;;
  esac
  ld_path=$(command -v "$gnu-ld.bfd" || true)
  if [ -z "$ld_path" ]; then
    echo "$0: no GNU ld for $gnu; install binutils-$gnu" >&2
    exit 1
  fi
}

# NetBSD and DragonFly ship libstdc++, and neither puts its headers where
# clang looks by default.  NetBSD keeps them together under /usr/include/g++;
# DragonFly splits them, with the headers proper under /usr/include/c++/<ver>
# and the target-specific bits/c++config.h off in /usr/libdata/gcc<ver>.
# Getting that wrong is not an error -- clang falls back to the host's headers
# and compiles Ubuntu's declarations against DragonFly's system headers, which
# comes apart much later as exception specifications that do not match.
# FreeBSD and OpenBSD ship libc++, which clang finds by itself.
case "$os" in
  netbsd)
    cxx_extra="-stdlib=libstdc++ -isystem $sysroot/usr/include/g++"
    rt_extra="--rtlib=libgcc"
    link_extra="-lgcc"
    # NetBSD's run-time linker maps a shared object itself, and up to and
    # including NetBSD 10 it handles exactly two PT_LOAD segments; anything
    # else it refuses, and reports as the file being missing.  lld emits four
    # and cannot be talked down to two while RELRO is kept, because it cuts
    # the writable segment at the RELRO boundary.  GNU ld does not, so with
    # -z noseparate-code -- which lld silently ignores, and which
    # flags-ldflags.m4 passes -- it produces the two segments NetBSD wants
    # and keeps the read-only relocations.  Measured on LLD 18.1.3 and GNU
    # ld 2.42.
    gnu_ld
    # clang asks for NetBSD's own emulation on 32-bit arm, and Debian's ld
    # was built with only the Linux ones, so it stops at
    #   unrecognised emulation mode: armelf_nbsd_eabihf
    # before reading an object.  The two differ in the default linker
    # script, and clang names the dynamic linker, the crt files and the
    # libraries itself, so the Linux emulation links the same thing.  The
    # other machines are asked for generic emulations that Debian has.
    # A long link line -- libjvm's -- reaches ld as a response file, so
    # the name is rewritten inside those too.  32-bit powerpc is the same
    # story, with elf32ppc_nbsd for elf32ppclinux.
    case "$gnu" in
      arm-linux-gnueabihf) nbsd_emul=armelf_nbsd_eabihf ; linux_emul=armelf_linux_eabi ;;
      powerpc-linux-gnu)   nbsd_emul=elf32ppc_nbsd ;      linux_emul=elf32ppclinux ;;
      *)                   nbsd_emul= ;;
    esac
    if [ -n "$nbsd_emul" ]; then
      cat > "$bindir/$triple-ld" <<W
#!/bin/sh
tmp=\$(mktemp -d)
trap 'rm -rf "\$tmp"' EXIT
n=0
for a; do
  shift
  case "\$a" in
    $nbsd_emul) a=$linux_emul ;;
    @*)
      if [ -f "\${a#@}" ]; then
        n=\$((n + 1))
        sed 's/$nbsd_emul/$linux_emul/g' "\${a#@}" > "\$tmp/\$n"
        a="@\$tmp/\$n"
      fi
      ;;
  esac
  set -- "\$@" "\$a"
done
$ld_path "\$@"
W
      chmod +x "$bindir/$triple-ld"
      ld_path="$bindir/$triple-ld"
    fi
    # The Zero targets call through libffi, which NetBSD ships in pkgsrc,
    # so it lands under usr/pkg rather than usr/lib.  Anything that links
    # against libjvm has to be able to find it a second time -- the gtest
    # launcher stops at "libffi.so.8 ... not found (try using -rpath or
    # -rpath-link)" -- and rpath-link is what answers that without putting
    # a build path into the binary.
    if [ -d "$sysroot/usr/pkg/lib" ]; then
      common_extra="-L$sysroot/usr/pkg/lib -Wl,-rpath-link=$sysroot/usr/pkg/lib"
    fi
    ;;
  dragonfly)
    cxxdir=$(ls -d "$sysroot"/usr/include/c++/*/ | sort -V | tail -1)
    cxxdir=${cxxdir%/}
    gccdir=$(ls -d "$sysroot"/usr/libdata/gcc*/ | sort -V | tail -1)
    gccdir=${gccdir%/}
    cxx_extra="-stdlib=libstdc++ -nostdinc++ -isystem $cxxdir \
        -isystem $cxxdir/backward -isystem $gccdir"
    rt_extra="--rtlib=libgcc"
    link_extra="-lgcc"
    # libffi, for Zero, is a package and unpacks under usr/local; see
    # OpenBSD below for the -rpath-link.
    common_extra="-L$sysroot/usr/local/lib -Wl,-rpath-link=$sysroot/usr/local/lib"
    ;;
  freebsd)
    cxx_extra=""
    rt_extra=""
    link_extra=""
    common_extra="-L$sysroot/usr/local/lib -Wl,-rpath-link=$sysroot/usr/local/lib"
    ;;
  openbsd)
    # iconv is a package there rather than part of the C library, and a
    # package unpacks under usr/local, which clang does not search.
    cxx_extra=""
    rt_extra=""
    link_extra=""
    # -rpath-link for the same reason as NetBSD above: libffi comes from a
    # package, and whatever links against libjvm has to find it again.
    common_extra="-isystem $sysroot/usr/local/include -L$sysroot/usr/local/lib \
        -Wl,-rpath-link=$sysroot/usr/local/lib"
    # lld cannot leave an R_SPARC_64 for the run-time linker to resolve, so
    # every vtable in libjvm stops the link with "relocation R_SPARC_64
    # cannot be used against symbol ...; recompile with -fPIC".  OpenBSD's
    # own sparc64 toolchain links with GNU ld, which emits them; do the same.
    case "$triple" in
      sparc64-*)
        ld_path=$(command -v sparc64-linux-gnu-ld.bfd || true)
        if [ -z "$ld_path" ]; then
          echo "$0: no GNU ld for sparc64; install binutils-sparc64-linux-gnu" >&2
          exit 1
        fi
        ;;
    # OpenBSD's mips64 ports still link their base system with GNU ld
    # 2.17, and lld will not read the libc.so that comes out of it:
    #   ld.lld: error: .../usr/lib/libc.so: invalid local symbol
    #     '_libc_memcmp' in global part of symbol table
    # 2.17 leaves local symbols after the first global one in .dynsym,
    # which lld rejects outright and GNU ld reads.  So these link with
    # Debian's GNU ld, as NetBSD does.  clang passes only -EB or -EL, which
    # the Linux ld takes as they are.
      mips64*) gnu_ld ;;
    esac
    ;;
  *)
    cxx_extra=""
    rt_extra=""
    link_extra=""
    ;;
esac
: "${common_extra:=}"
: "${ld_path:=}"
if [ -n "$ld_path" ]; then ld_flag="--ld-path=$ld_path"; else ld_flag="-fuse-ld=lld"; fi

for tool in clang clang++; do
  case "$tool" in
    clang++) extra="$cxx_extra" ;;
    *)       extra="" ;;
  esac
  cat > "$bindir/$triple-$tool" <<W
#!/bin/sh
# -Wno-unused-command-line-argument: the linker flags below are passed on
# every invocation, including the compile-only ones, and the JDK builds
# with warnings as errors.
exec /usr/bin/$tool$llvm_suffix --target=$triple --sysroot=$sysroot \\
  -Wno-unused-command-line-argument \\
  $rt_extra $ld_flag $common_extra $extra \\
  -include $fixups "\$@" $link_extra
W
  chmod +x "$bindir/$triple-$tool"
done

# Wrappers rather than symlinks: the LLVM binutils pick their mode out of
# the name they are invoked by, and the FreeBSD triple has a version in it,
# so llvm-ar called as x86_64-unknown-freebsd15.1-ar reads the ".1-ar" as a
# suffix and refuses -- "error: not ranlib, ar, lib or dlltool".
for tool in ar ranlib strip objcopy nm objdump; do
  printf '#!/bin/sh\nexec /usr/bin/llvm-%s%s "$@"\n' "$tool" "$llvm_suffix" > "$bindir/$triple-$tool"
  chmod +x "$bindir/$triple-$tool"
done

# NetBSD unwinds with DWARF CFI on 32-bit arm, not the ARM EHABI, and its
# libraries carry no __cxa_end_cleanup: a sysroot search found it in none
# of libstdc++, libsupc++, libgcc, libgcc_eh, libgcc_s or libc.  LLVM
# nevertheless ended every C++ cleanup with a call to it on any *eabi*
# triple, NetBSD's included, until Triple::isTargetEHABICompatible learnt
# to leave NetBSD out -- and an executable with such a call does not link:
#   undefined reference to `__cxa_end_cleanup'
# while a shared library links and fails when it is loaded.  Compile one
# cleanup here and stop now if this clang still does it, rather than at the
# first C++ executable forty minutes into the build.
case "$triple" in
  armv7-*netbsd*)
    printf 'struct S { ~S(); };\nvoid g();\nvoid f() { S s; g(); }\n' > "$bindir/eh-probe.cpp"
    if "$bindir/$triple-clang++" -O2 -S -o - "$bindir/eh-probe.cpp" |
        grep -q __cxa_end_cleanup; then
      echo "$0: this clang ends C++ cleanups with __cxa_end_cleanup on" >&2
      echo "$0: $triple, which NetBSD does not provide; use a newer LLVM" >&2
      exit 1
    fi
    rm -f "$bindir/eh-probe.cpp"
    ;;
esac

# Prove the wrapper links before configure spends ten minutes finding out.
tmp=$(mktemp -d)
printf 'int main(void){return 0;}\n' > "$tmp/probe.c"
"$bindir/$triple-clang" "$tmp/probe.c" -o "$tmp/probe"
file "$tmp/probe"
rm -rf "$tmp"

# Zero calls through libffi, and gen-bsd-sysroot.sh takes it from the
# OS's packages where there are any.  Some machines have none -- FreeBSD
# 15 builds packages for amd64 and aarch64 only, pkgsrc skips NetBSD's
# N64 mips ports, OpenBSD 7.9 has none at all for loongson -- and for
# those it is built here, with the wrappers just written, into the
# directory the packages would have used.  OpenBSD's libiconv, which
# java.instrument and libjdwp need, is the same.  Done after the sysroot
# is cached is fine: the cache is saved at the end of the job, with
# these in it.
case "$os" in
  netbsd) prefix=/usr/pkg ;;
  *)      prefix=/usr/local ;;
esac
build_from_source() {
  local name="$1" url="$2"
  shift 2
  echo "building $name for $triple from $url"
  local src
  src=$(mktemp -d)
  curl -fsSL --retry 3 --retry-all-errors --max-time 600 -o "$src.tar.gz" "$url"
  tar xzf "$src.tar.gz" -C "$src" --strip-components=1
  rm -f "$src.tar.gz"
  (
    cd "$src"
    # libffi knows MIPS on Linux, OpenBSD and FreeBSD and stops anywhere
    # else with "libffi has not been ported to mips64-unknown-netbsd",
    # although the code is the same.  pkgsrc patches NetBSD into the same
    # line; so does this.  configure reads configure.host when it runs.
    if [ "$name" = libffi ]; then
      sed -i 's/mips\*-\*-openbsd\* |/mips*-*-openbsd* | mips*-*-netbsd* |/' configure.host
    fi
    ./configure --host="$triple" --prefix="$prefix" \
        CC="$bindir/$triple-clang" AR="$bindir/$triple-ar" \
        RANLIB="$bindir/$triple-ranlib" STRIP="$bindir/$triple-strip" \
        NM="$bindir/$triple-nm" "$@" > configure.out 2>&1 ||
        { cat configure.out config.log; exit 1; }
    make -j"$(nproc)" > make.out 2>&1 || { tail -100 make.out; exit 1; }
    make install DESTDIR="$sysroot" > /dev/null
  )
  rm -rf "$src"
  # libtool's .la files name the build's paths; nothing here reads them.
  rm -f "$sysroot$prefix"/lib/*.la
  # As in gen-bsd-sysroot.sh: OpenBSD's libtool installs libfoo.so.N.M
  # alone, and lld wants a libfoo.so to find.
  for so in "$sysroot$prefix"/lib/lib*.so.*; do
    [ -e "$so" ] || continue
    stem=${so%%.so.*}
    [ -e "$stem.so" ] || ln -sf "$(basename "$so")" "$stem.so"
  done
}
if [ ! -e "$sysroot$prefix/include/ffi.h" ] && [ ! -e "$sysroot/usr/include/ffi.h" ]; then
  build_from_source libffi \
      https://github.com/libffi/libffi/releases/download/v3.5.2/libffi-3.5.2.tar.gz \
      --disable-docs --disable-multi-os-directory
fi
if [ "$os" = openbsd ] && [ ! -e "$sysroot$prefix/include/iconv.h" ]; then
  build_from_source libiconv https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.18.tar.gz
fi
