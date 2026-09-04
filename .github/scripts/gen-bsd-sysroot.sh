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
# Usage: gen-bsd-sysroot.sh <netbsd|freebsd|openbsd|dragonfly> <directory>

set -eu

os="$1"
sysroot="$2"
mkdir -p "$sysroot"

fetch() {
  curl -fsSL --retry 3 --retry-delay 5 --retry-all-errors -o "$1" "$2"
}

case "$os" in
  netbsd)
    # comp holds the headers and the static libraries; xbase and xcomp hold
    # X11, which java.desktop needs and which NetBSD ships apart from base.
    base=https://cdn.netbsd.org/pub/NetBSD/NetBSD-11.0/amd64/binary/sets
    for set in base comp xbase xcomp; do
      fetch "$set.tar.xz" "$base/$set.tar.xz"
      sudo tar xJf "$set.tar.xz" -C "$sysroot"
    done
    ;;

  freebsd)
    # FreeBSD puts everything in one base.txz, X11 included as headers only.
    base=https://download.freebsd.org/releases/amd64/15.1-RELEASE
    fetch base.txz "$base/base.txz"
    sudo tar xJf base.txz -C "$sysroot"
    ;;

  openbsd)
    # OpenBSD numbers its sets after the release: base79.tgz for 7.9.  comp
    # carries the headers, xbase and xshare the X11 side.
    base=https://cdn.openbsd.org/pub/OpenBSD/7.9/amd64
    for set in base79 comp79 xbase79 xshare79; do
      fetch "$set.tgz" "$base/$set.tgz"
      sudo tar xzf "$set.tgz" -C "$sysroot"
    done
    ;;

  dragonfly)
    # DragonFly ships its base as one image rather than as sets, so the
    # sysroot comes out of the release ISO's own base tarball.
    base=https://mirror-master.dragonflybsd.org/iso-images
    fetch dfly.tar.bz2 "$base/dfly-x86_64-6.4.2_REL.tar.bz2"
    sudo tar xjf dfly.tar.bz2 -C "$sysroot"
    ;;

  *)
    echo "gen-bsd-sysroot.sh: unknown target $os" >&2
    exit 1
    ;;
esac

sudo chown -R "$USER" "$sysroot"

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
rm -rf "$sysroot"/usr/{sbin,share,libdata,libexec} 2>/dev/null || true
rm -rf "$sysroot"/{sbin,bin,rescue} 2>/dev/null || true

ls -d "$sysroot/usr/include" "$sysroot/usr/lib"
