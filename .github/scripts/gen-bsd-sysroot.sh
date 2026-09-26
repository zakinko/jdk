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

# Fetches a sysroot for one of the BSDs from that operating system's own
# distribution sets.  There is no debootstrap here: each BSD publishes its
# base system as a handful of tarballs, and the pieces the JDK needs are the
# base system, the compiler support files and, where the X11 headers are
# packaged apart from the rest, those too.
#
# Usage: gen-bsd-sysroot.sh <netbsd|freebsd|openbsd|dragonfly> <directory> [arch]
#
# arch is spelled the way the JDK spells it -- x86_64, aarch64, powerpc64,
# i386, sparc64 -- and each case below translates that into whatever the
# operating system calls the same machine on its own mirror.  They disagree
# with each other and, in OpenBSD's case, with themselves.

set -eu

os="$1"
sysroot="$2"
arch="${3:-x86_64}"
mkdir -p "$sysroot"

unsupported() {
  echo "gen-bsd-sysroot.sh: $os has no $arch sysroot here" >&2
  exit 1
}

# --max-time so that a mirror that accepts the connection and then stops
# sending fails the job rather than sitting there until the six hour
# runner limit.
fetch() {
  echo "fetching $2"
  curl -fsSL --retry 3 --retry-delay 5 --retry-all-errors \
      --max-time 1800 --speed-limit 10240 --speed-time 120 -o "$1" "$2"
  ls -l "$1"
}

extract() {
  echo "extracting $1"
  sudo tar xf "$1" -C "$sysroot" "${@:2}"
}

case "$os" in
  netbsd)
    # comp holds the headers and the static libraries.  The X11 sets are not
    # fetched: the build is headless, see below.
    #
    # Build against the released version, not the newest.  NetBSD 11's
    # <pthread.h> resolves pthread_attr_destroy to __libc_thr_attr_destroy,
    # a symbol NetBSD 10 does not export, so a JDK built against 11 dies on
    # 10 the moment the launcher dlopens libjvm.so:
    #
    #   Undefined PLT symbol "__libc_thr_attr_destroy" (symnum = 21)
    #
    # 10 is also as far back as this source builds -- a 9.4 sysroot stops in
    # os_posix.cpp, where PTHREAD_STACK_MIN is undeclared until 10 -- and it
    # is the version vmactions offers, so it is what gets tested.
    netbsd_release=10.1
    # NetBSD names a port after the board family where the CPU is not the
    # whole story, so aarch64 lives under evbarm-aarch64.  The sets are xz
    # everywhere except i386, which 10.1 still ships gzipped.
    # pkgsrc files its binary packages by MACHINE_ARCH, which differs from
    # the port name wherever the port is a board family.
    case "$arch" in
      x86_64)  port=amd64 ;           ext=tar.xz ; pkgarch=x86_64 ;;
      aarch64) port=evbarm-aarch64 ;  ext=tar.xz ; pkgarch=aarch64 ;;
      sparc64) port=sparc64 ;         ext=tar.xz ; pkgarch=sparc64 ;;
      i386)    port=i386 ;            ext=tgz ;    pkgarch=i386 ;;
      armv7)   port=evbarm-earmv7hf ; ext=tgz ;    pkgarch=earmv7hf ;;
      # There is no riscv-riscv64 release before 11.0, so the 10.1 pin
      # above has nothing to hold on to here.
      riscv64) port=riscv-riscv64 ;   ext=tgz ;    pkgarch=riscv64
               netbsd_release=11.0 ;;
      *) unsupported ;;
    esac
    base=https://cdn.netbsd.org/pub/NetBSD/NetBSD-$netbsd_release/$port/binary/sets
    # NetBSD is the one BSD here that ships X11 as ordinary sets, so a
    # headful build can be cross-compiled for it.  They cost about 20MB
    # and a headless build simply does not look at them.
    for set in base comp xbase xcomp; do
      fetch "$set.$ext" "$base/$set.$ext"
      extract "$set.$ext"
    done
    # The machines with no HotSpot port on BSD are built as Zero, which
    # calls through libffi.  NetBSD keeps it in pkgsrc rather than base,
    # and a binary package unpacks relative to /usr/pkg.
    case "$arch" in
      i386|sparc64|armv7|riscv64)
        pkgs=https://cdn.netbsd.org/pub/pkgsrc/packages/NetBSD/$pkgarch/$netbsd_release/All
        fetch libffi.tgz "$pkgs/libffi-3.5.2.tgz"
        sudo mkdir -p "$sysroot/usr/pkg"
        echo "extracting libffi.tgz into usr/pkg"
        sudo tar xf libffi.tgz -C "$sysroot/usr/pkg" include lib
        ;;
    esac
    ;;

  freebsd)
    # FreeBSD puts the whole base system in one base.txz.
    # amd64 is the one release directory that is not <target>/<target_arch>.
    case "$arch" in
      x86_64)    relpath=amd64 ;;
      aarch64)   relpath=arm64/aarch64 ;;
      powerpc64) relpath=powerpc/powerpc64 ;;
      powerpc64le) relpath=powerpc/powerpc64le ;;
      *) unsupported ;;
    esac
    base=https://download.freebsd.org/releases/$relpath/15.1-RELEASE
    fetch base.txz "$base/base.txz"
    extract base.txz
    ;;

  openbsd)
    # OpenBSD numbers its sets after the release: base79.tgz for 7.9, with
    # comp79 carrying the headers.
    # The same mirror spells this machine two ways: the release sets are
    # under arm64/ and the packages under aarch64/.
    case "$arch" in
      x86_64)  setdir=amd64 ;   pkgdir=amd64 ;;
      aarch64) setdir=arm64 ;   pkgdir=aarch64 ;;
      sparc64) setdir=sparc64 ; pkgdir=sparc64 ;;
      i386)    setdir=i386 ;    pkgdir=i386 ;;
      powerpc64) setdir=powerpc64 ; pkgdir=powerpc64 ;;
      armv7)   setdir=armv7 ;   pkgdir=arm ;;
      riscv64) setdir=riscv64 ; pkgdir=riscv64 ;;
      *) unsupported ;;
    esac
    base=https://cdn.openbsd.org/pub/OpenBSD/7.9/$setdir
    for set in base79 comp79; do
      fetch "$set.tgz" "$base/$set.tgz"
      extract "$set.tgz"
    done
    # Alone among these systems, OpenBSD has no iconv in its C library, and
    # java.instrument and libjdwp include <iconv.h> outright.  It comes from
    # a package, which unpacks under usr/local.
    # A package's paths are relative to /usr/local, so it needs its own
    # destination rather than the sysroot root.
    fetch libiconv.tgz \
        https://cdn.openbsd.org/pub/OpenBSD/7.9/packages/$pkgdir/libiconv-1.19.tgz
    sudo mkdir -p "$sysroot/usr/local"
    echo "extracting libiconv.tgz into usr/local"
    sudo tar xf libiconv.tgz -C "$sysroot/usr/local"
    # The Zero machines call through libffi, which is a package here too.
    case "$arch" in i386|sparc64|armv7|riscv64)
      fetch libffi.tgz \
          https://cdn.openbsd.org/pub/OpenBSD/7.9/packages/$pkgdir/libffi-3.5.2p0.tgz
      echo "extracting libffi.tgz into usr/local"
      sudo tar xf libffi.tgz -C "$sysroot/usr/local" include lib
      ;;
    esac
    ;;

  dragonfly)
    # DragonFly publishes no distribution sets: the release is an ISO or a
    # disk image and nothing else.  The ISO is cd9660 and libarchive reads
    # that directly, so the headers and libraries come straight out of it
    # without a loop mount.
    # DragonFly is x86_64 only.
    [ "$arch" = x86_64 ] || unsupported
    base=https://mirror-master.dragonflybsd.org/iso-images
    fetch dfly.iso.bz2 "$base/dfly-x86_64-6.4.2_REL.iso.bz2"
    bunzip2 dfly.iso.bz2
    # No usr/libdata/ldscripts: the ISO does not carry it, and lld does not
    # read linker scripts from the sysroot anyway.
    # usr/libdata carries bits/c++config.h, which sits apart from the rest
    # of the libstdc++ headers on DragonFly.
    bsdtar -xf dfly.iso -C "$sysroot" usr/include usr/lib usr/libdata lib
    rm -f dfly.iso
    ;;

  *)
    echo "gen-bsd-sysroot.sh: unknown target $os" >&2
    exit 1
    ;;
esac

sudo chown -R "$USER" "$sysroot"


# No BSD carries X11 in its base system: NetBSD and OpenBSD ship it as
# separate sets and the others leave it to ports.  NetBSD's are fetched
# above, so a headful build is possible there; everywhere else the build is
# configured headless, which gives up the X11 half of java.desktop and
# nothing else -- hotspot included, all of it still gets compiled.
#
# Neither cups nor fontconfig is part of any BSD base system, and the JDK
# needs their headers alone: both are opened with dlopen at run time.  The
# Linux ones say the same thing.
for h in cups fontconfig; do
  if [ ! -d "$sysroot/usr/include/$h" ] && [ -d "/usr/include/$h" ]; then
    cp -R "/usr/include/$h" "$sysroot/usr/include/"
  fi
done

# Drop what the build never reads, so that the cache entry stays small.
rm -rf "$sysroot"/{dev,proc,var,tmp,root,home} 2>/dev/null || true
rm -rf "$sysroot"/usr/{sbin,share,libexec} 2>/dev/null || true
rm -rf "$sysroot"/{sbin,bin,rescue} 2>/dev/null || true

# Every one of these systems keeps its linker symlinks absolute --
# /usr/lib/libc.so points at /lib/libc.so.N -- and a symlink is resolved by
# the kernel, which knows nothing about --sysroot.  Left alone they reach out
# of the sysroot and into the host, and the link either fails or, worse,
# succeeds against the wrong libc.  Make them relative.
#
# After the pruning above, not before: a distribution set carries directories
# the build never looks at and whose modes stop even the owner walking them
# (var/spool/ftp/hidden, dev), and those are gone by now.
sudo chmod -R u+rwX "$sysroot"
find "$sysroot" -type l | while read -r link; do
  target=$(readlink "$link")
  case "$target" in
    /*) ;;
    *) continue ;;
  esac
  up=$(dirname "${link#$sysroot/}" | awk -F/ '{ for (i = 1; i <= NF; i++) printf "../" }')
  ln -sf "${up}${target#/}" "$link"
done

# OpenBSD ships no unversioned libfoo.so.  Its own linker scans the directory
# and takes the highest libfoo.so.N.M it finds; lld does not, so -lc++abi
# falls through to libc++abi.a and the link of a shared object dies on
#
#   ld.lld: error: relocation R_X86_64_PC32 cannot be used against symbol
#     '__cxa_new_handler'; recompile with -fPIC
#
# because the archive is not built PIC.  Make the symlinks lld expects.
for dir in "$sysroot"/usr/lib "$sysroot"/lib "$sysroot"/usr/pkg/lib \
           "$sysroot"/usr/local/lib "$sysroot"/usr/X11R7/lib; do
  [ -d "$dir" ] || continue
  for so in "$dir"/lib*.so.*; do
    [ -e "$so" ] || continue
    stem=${so%%.so.*}
    [ -e "$stem.so" ] && continue
    newest=$(ls -1 "$stem".so.* 2>/dev/null | sort -V | tail -1)
    ln -sf "$(basename "$newest")" "$stem.so"
  done
done

ls -d "$sysroot/usr/include" "$sysroot/usr/lib"
