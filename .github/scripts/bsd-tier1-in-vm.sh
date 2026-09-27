#!/bin/sh
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

# Runs one tier1 part inside a BSD virtual machine, against the bundles a
# cross build uploaded, from the root of the workspace the VM was handed.
# test-bsd.yml boots the machine; this is what it runs there, the same on
# every BSD but for what each one needs of a JDK it did not build itself.
#
# Usage: bsd-tier1-in-vm.sh <test-suite>
#
# It returns 0 when the tests ran, whatever they found: the VM actions copy
# the workspace back out only when their step succeeds, so a failing exit
# here would throw away the files the failure has to be read from.
# test-bsd.yml reads make's verdict on the runner afterwards.

set -e
suite="$1"
PATH=/usr/pkg/bin:/usr/pkg/sbin:/usr/local/bin:/usr/local/sbin:$PATH
export PATH

JDK="$PWD/bundles/jdk/`ls bundles/jdk`"
TESTS="$PWD/bundles/tests"
JT="$PWD/jtreg/installed"
os=`uname -s`

if [ "$os" = NetBSD ]; then
  # A JDK built on NetBSD is marked as it is linked; one cross-built from
  # Linux cannot be, because paxctl(8) only exists here.  Without the mark
  # every launcher dies in VM initialisation, because PaX refuses the
  # writable and executable mapping the JIT asks for.
  find "$JDK" "$TESTS" -type f \( -perm -u+x -o -name '*.so' \) -print |
    while read f; do /usr/sbin/paxctl +m "$f" >/dev/null 2>&1 || :; done

  # The launchers inside the jmods need it too.  jlink assembles a new
  # image out of those, not out of bin/, so marking only the unpacked tree
  # leaves every image a test builds unmarked, and it dies before running a
  # line with "Could not reserve enough space in CodeHeap", which is PaX
  # MPROTECT refusing the JIT's mapping, not a size.
  #
  # A jmod is a zip behind a four-byte header.  Replace only the entries
  # that need marking and pass -D, because the jmod reader rejects
  # directory entries that a plain rezip would add.  Most jmods carry no
  # launcher; those are left alone.
  echo "--- marking the launchers inside the jmods ---"
  WORK="$PWD/jmodwork"
  for m in "$JDK"/jmods/*.jmod; do
    rm -rf "$WORK"; mkdir -p "$WORK"
    dd if="$m" bs=1 count=4 of="$WORK/magic" 2>/dev/null
    dd if="$m" bs=4 skip=1 of="$WORK/body.zip" 2>/dev/null
    ( cd "$WORK" && unzip -q -o body.zip 'bin/*' ) >/dev/null 2>&1 || continue
    [ -d "$WORK/bin" ] || continue
    n=0
    for f in "$WORK"/bin/*; do
      [ -f "$f" ] || continue
      /usr/sbin/paxctl +m "$f" >/dev/null 2>&1 && n=`expr $n + 1`
    done
    [ "$n" -gt 0 ] || continue
    ( cd "$WORK" && zip -q -D -X body.zip bin/* ) || continue
    cat "$WORK/magic" "$WORK/body.zip" > "$m.new" && mv "$m.new" "$m"
    echo "  $n in ${m##*/}"
  done
  rm -rf "$WORK"
fi

if [ "$os" = OpenBSD ]; then
  # The JVM reserves its heap and code cache up front, well past the
  # default data size limit of a login class.
  ulimit -Sd `ulimit -Hd`
fi

# Build the smallest image jlink can make and start it.  Eight tier1 tests
# build an image and run it, and anything that stops a jlinked launcher --
# on NetBSD an unmarked one -- fails all eight an hour later as a CodeHeap
# message.  Say so here instead, with what jlink printed.
"$JDK/bin/jlink" --module-path "$JDK/jmods" --add-modules java.base \
    --output "$PWD/probeimage" > "$PWD/jlink.log" 2>&1 || :
if [ -x "$PWD/probeimage/bin/java" ]; then
  if [ "$os" = NetBSD ]; then
    /usr/sbin/paxctl "$PWD/probeimage/bin/java"
    /usr/sbin/paxctl "$PWD/probeimage/bin/java" | grep -q 'mprotect' || {
      echo "a jlink-built image comes out unmarked; the eight image"
      echo "tests will fail in VM initialisation"
      exit 1
    }
  fi
  "$PWD/probeimage/bin/java" -version 2>&1 | head -2
else
  echo "jlink could not build a probe image; it said:"
  sed 's/^/  /' "$PWD/jlink.log" | head -40
  for e in "$PWD"/hs_err_pid*.log; do
    [ -f "$e" ] || continue
    echo "--- $e ---"
    head -30 "$e" | sed 's/^/  /'
  done
  exit 1
fi
rm -rf "$PWD/probeimage"

# What the launcher asks the run-time linker for, so a missing library is
# named here rather than read out of a test log.
echo "--- what the VM sees ---"
uname -a
readelf -d "$JDK/bin/java" 2>/dev/null | grep -E 'RPATH|RUNPATH|NEEDED' ||
  objdump -p "$JDK/bin/java" 2>/dev/null | grep -E 'RPATH|RUNPATH|NEEDED' || :
ldd "$JDK/bin/java" 2>&1 | head -8
echo "--- end ---"

"$JDK/bin/java" -version

# The makefiles are written for GNU sed and grep; where the base system's
# are the BSD ones, the packaged GNU ones are named instead.
gnu=""
if command -v gsed >/dev/null 2>&1; then gnu="$gnu SED=`command -v gsed`"; fi
if command -v ggrep >/dev/null 2>&1; then gnu="$gnu GREP=`command -v ggrep`"; fi

gmake test-prebuilt $gnu \
  TEST="$suite" \
  BOOT_JDK="$JDK" \
  JT_HOME="$JT" \
  JDK_IMAGE_DIR="$JDK" \
  TEST_IMAGE_DIR="$TESTS" \
  JTREG="JAVA_OPTIONS=-XX:-CreateCoredumpOnCrash;VERBOSE=fail,error,time;KEYWORDS=!headful;TIMEOUT_FACTOR=${TIMEOUT_FACTOR:-4}"

# make test-prebuilt prints "TEST FAILURE" and then returns 0: it reports
# the failure as build/run-test-prebuilt/make-support/exit-with-error.
# Name what failed here, where the log is read; the verdict is passed on
# the runner.
R=build/run-test-prebuilt
if [ -f $R/make-support/exit-with-error ]; then
  echo "--- tests that failed ---"
  cat $R/test-results/*/text/newfailures.txt 2>/dev/null || :
  echo "--- tests that errored ---"
  cat $R/test-results/*/text/other_errors.txt 2>/dev/null || :
  # Why each one failed, at the very end of the log: the log is read from
  # its tail, and a part's own output runs to more lines than can be
  # fetched, so the reason would otherwise be out of reach.
  cat $R/test-results/*/text/newfailures.txt \
      $R/test-results/*/text/other_errors.txt 2>/dev/null |
    grep -v '^#' | sed 's/[#].*//' | sort -u | head -40 |
    while read t; do
      [ -n "$t" ] || continue
      jtr=`find $R/test-support -path "*/${t%.*}*.jtr" 2>/dev/null | head -1`
      [ -n "$jtr" ] || continue
      echo "--- $t ---"
      grep -E 'Exception|Error|FAILED|failed|expected|timed out|^TEST RESULT' "$jtr" |
        grep -v '^[[:space:]]*at ' | head -15
    done
  echo "--- end ---"
fi
exit 0
