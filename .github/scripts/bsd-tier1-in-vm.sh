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

if [ -f bundles.sha256 ] && command -v sha256sum >/dev/null 2>&1; then
  # A copy the guest damaged fails later in ways that look like JDK bugs --
  # a SIGILL in libjvm, a class file with a bad magic number.  Say so here.
  rc=0
  sums=`cd bundles && sha256sum -c ../bundles.sha256 2>&1` || rc=$?
  if [ $rc -ne 0 ]; then
    echo "$sums" | grep -v ': OK$' | head -20
    echo "the JDK bundle arrived in the guest damaged; not running the tests"
    exit 1
  fi
  echo "bundle checksums match" >> "$PWD/setup.txt"
fi

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

if [ "$os" = DragonFly ]; then
  # The image's root is HAMMER2, which keeps a truncated file's last 64K
  # buffer in memory, so a mapping of the part cut off still reads, where
  # every other file system raises SIGBUS.  runtime/Unsafe/InternalErrorTest
  # ("InternalError not thrown") and java/foreign/sharedclosejfr/
  # TestSharedCloseJFR ("InternalError was expected") test for exactly that
  # fault, and tools/javac/6394683/T6394683 stops at "Cannot create files"
  # on the same file system.  The tests work in build/, so put that on
  # tmpfs, which behaves as the tests expect.
  mkdir -p build
  if mount_tmpfs tmpfs build; then
    echo "build/ is on tmpfs" >> "$PWD/setup.txt"
  else
    echo "could not put build/ on tmpfs; the tests run on HAMMER2" | tee -a "$PWD/setup.txt"
  fi
  # tools/javac/6394683/T6394683 rewrites a file once a second for five
  # seconds and still finds its mtime no newer than an older file's
  # ("Cannot create files"), on HAMMER2 and on tmpfs alike.  Record how this
  # machine stamps files and keeps time, to see which of the two is behind.
  {
    sysctl vfs.timestamp_precision kern.timecounter.hardware 2>&1 || :
    t=build/mtime-probe
    echo a > $t.1; sleep 1; echo b > $t.2; sleep 1; echo c > $t.1
    echo "mtime probe: first `stat -f %m $t.1` second `stat -f %m $t.2` (rewritten after it)"
    : > $t.3; m1=`stat -f %m $t.3`; sleep 2; : > $t.3; m2=`stat -f %m $t.3`
    echo "mtime probe: empty file truncated again 2s later: $m1 -> $m2"
    rm -f $t.1 $t.2 $t.3
  } >> "$PWD/setup.txt" 2>&1
fi

if [ "$os" = OpenBSD ]; then
  # The JVM reserves its heap and code cache up front, well past the
  # default data size limit of a login class.  Raising the soft limit to
  # the hard one was not enough: the jdk parts that size the heap from the
  # machine still stopped at
  #   os::commit_memory(0x0000000781000000, 2130706432, 0) failed;
  #   error='ENOMEM' (errno=12)
  # The steps run as root, which may lift the hard limit too.
  ulimit -d unlimited 2>/dev/null || ulimit -Sd `ulimit -Hd`
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
# The vector length C2 sizes its code for, read on this machine.  FreeBSD
# runs every thread at the largest length the CPU offers, and under QEMU
# that can be up to 256 bytes, where Linux starts a thread at 64.
"$JDK/bin/java" -XX:+PrintFlagsFinal -version 2>/dev/null |
  grep -E ' (UseSVE|MaxVectorSize|UseSIMDForMemoryOps|UseAVX) ' |
  tee -a "$PWD/setup.txt" || :

# A cross build leaves out the default CDS archive -- jdk-options.m4 turns
# --enable-cds-archive off for cross compilation, since the build cannot run
# what it built -- and a JDK built on the BSD itself would carry one.  Dump
# it here, as Images.gmk would have, so the tests that assume lib/server/
# classes.jsa (NonJVMVariantLocation and TestCDSVMCrash start with
# -Xshare:on) test the JDK a native build makes.  The VM has to be what
# dumps it: the archive records the running JVM.
"$JDK/bin/java" -Xshare:dump -XX:SharedArchiveFile="$JDK/lib/server/classes.jsa" \
    -Xmx128M -Xms128M > "$PWD/cds-dump.log" 2>&1 ||
  { echo "the default CDS archive could not be dumped:"; tail -20 "$PWD/cds-dump.log"; }

# The makefiles are written for GNU sed and grep; where the base system's
# are the BSD ones, the packaged GNU ones are named instead.
gnu=""
if command -v gsed >/dev/null 2>&1; then gnu="$gnu SED=`command -v gsed`"; fi
if command -v ggrep >/dev/null 2>&1; then gnu="$gnu GREP=`command -v ggrep`"; fi

# A part split across SHARDS jobs runs every SHARDS-th test file of it,
# starting at the SHARD-th, and leaves the rest to the other jobs by listing
# them as problems.  jtreg -l names the tests the part selects, after the
# keywords and @requires, so the split is of what would really run.
extra=""
if [ "${SHARDS:-1}" -gt 1 ]; then
  root=${suite%%:*}
  "$JDK/bin/java" -jar "$JT/lib/jtreg.jar" -l -jdk:"$JDK" -k:'!headful' \
      "$PWD/${root%/}:${suite#*:}" > "$PWD/shard-all.txt" 2>&1 || :
  grep -E '\.(java|sh|html)(#.*)?$' "$PWD/shard-all.txt" |
    sort -u > "$PWD/shard-tests.txt"
  sed 's/#.*//' "$PWD/shard-tests.txt" | sort -u > "$PWD/shard-files.txt"
  n=`wc -l < "$PWD/shard-files.txt"`
  if [ "$n" -gt 0 ]; then
    # Split by file, so the variants of one test stay together, but list
    # every variant to exclude by its own name: an entry for Foo.java does
    # not exclude Foo.java#id, and the variants ran in every shard.
    awk -v k="$SHARD" -v n="$SHARDS" '
        NR == FNR { mine[$0] = ((FNR - 1) % n == k - 1); next }
        { f = $0; sub(/#.*/, "", f); if (!mine[f]) print $0 " 0000000 generic-all" }' \
        "$PWD/shard-files.txt" "$PWD/shard-tests.txt" > "$PWD/shard-exclude.txt"
    m=`sed 's/#.*//' "$PWD/shard-exclude.txt" | sort -u | wc -l`
    echo "shard $SHARD of $SHARDS: `expr $n - $m` of $n test files" | tee -a "$PWD/setup.txt"
    cat "$PWD/shard-exclude.txt" >> "$PWD/extra-problems.txt"
  else
    { echo "could not list the tests of $suite, so running all of them; jtreg said:"
      tail -5 "$PWD/shard-all.txt"; } | tee -a "$PWD/setup.txt"
  fi
fi

if [ "$os" = DragonFly ]; then
  # DragonFly lets a mapping read on past the end of a file that has been
  # truncated under it, where POSIX has the access raise SIGBUS: these two
  # map a file, truncate it and expect the read to fault, and fail with
  #   java.lang.RuntimeException: InternalError not thrown
  #   java.lang.RuntimeException: InternalError was expected
  # on HAMMER2 and on tmpfs alike.  No signal arrives for the VM to turn
  # into the InternalError, so there is nothing for it to do.
  cat >> "$PWD/extra-problems.txt" <<'P'
runtime/Unsafe/InternalErrorTest.java 0000000 generic-all
java/foreign/sharedclosejfr/TestSharedCloseJFR.java 0000000 generic-all
P
  # tools/javac/6394683/T6394683 makes its "newer" B.class by opening the
  # existing, empty file with O_TRUNC and closing it, once a second, and
  # stops with "Cannot create files" when the mtime never moves.  A file
  # written with content does get a new mtime here (the probe above), so
  # DragonFly appears to leave the mtime of an empty file alone when it is
  # truncated to the size it already has, where POSIX open(2) says O_TRUNC
  # marks it for update.  The second probe line records which.
  echo "tools/javac/6394683/T6394683.java 0000000 generic-all" >> "$PWD/extra-problems.txt"
  # jdk/tier1 part 1 takes the whole VM down: ssh stops answering about
  # three minutes after java/lang/ProcessHandle/InfoTest passes, every time,
  # and nothing comes back to say which test was running.  One test at a
  # time, so the last name in the log is the one that does it.
  case "$suite" in *tier1_part1) extra="$extra;JOBS=1" ;; esac
  # With one test at a time the machine still dies right after
  # java/lang/ProcessHandle/InfoTest passes, and the test that runs next is
  # java/lang/ProcessHandle/OnExitTest, which starts and kills trees of
  # processes and waits for each to exit.
  echo "java/lang/ProcessHandle/OnExitTest.java 0000000 generic-all" >> "$PWD/extra-problems.txt"
fi
case `uname -m` in
  arm64|aarch64|evbarm)
    # The aarch64 guests run under QEMU without acceleration -- the arm
    # runners have no /dev/kvm -- and on all three BSDs the same tests stop
    # making progress there and run into their timeouts, while the x86_64
    # guests of the same BSDs pass them:
    #   JSR166TestCase: "ForkJoin common pool thread stuck", 120s per case
    #   ParkALot: "=> 1631 of 300000" for four hours, Thread-1 in yield0
    #   NotifiedThenTimedOutWait, MiscMonitorTests waitNotifyTest,
    #   both Starvation tests, StructuredTaskScopeTest,
    #   LotsOfContendedMonitorEnter, GetStackTrace*StressTest,
    #   MonitorEnterExit#Xcomp (testMutualExclusion, every ForkJoinPool
    #   worker parked in awaitWork), forkjoin/SubmissionTest ("Submitted
    #   task failed to execute"), GatherersMapConcurrentTest,
    #   MonitorWaitNotify#Xcomp-noTieredCompilation, and in the virtual
    #   thread stress directory PingPong#ltq, TimedWaitALot and whatever runs
    #   after GetStackTraceALotWithTimedWait, which stalled jdk part 1 shard 4
    #   for four hours on FreeBSD, NetBSD and OpenBSD alike,
    #   CancelTimerWithContention (96 minutes, NetBSD) and
    #   Exchanger/ExchangeLoops (24 minutes, OpenBSD)
    # One of them is enough to take a shard past the six-hour job limit,
    # and the rest of its tests with it.  Whether the emulation or the port
    # is to blame is not settled; leave them out here so that the rest is
    # tested, and settle it on hardware.
    if [ -s "$PWD/shard-tests.txt" ]; then
      grep -E '(JSR166TestCase|ParkALot|NotifiedThenTimedOutWait|MiscMonitorTests|Starvation|StructuredTaskScopeTest|LotsOfContendedMonitorEnter|GetStackTrace[A-Za-z]*StressTest|MonitorEnterExit|MonitorWaitNotify|CancelTimerWithContention|Thread/virtual/stress/[A-Za-z]*|forkjoin/SubmissionTest|GatherersMapConcurrentTest|Exchanger/ExchangeLoops)\.java' \
          "$PWD/shard-tests.txt" | sed 's/$/ 0000000 generic-all/' >> "$PWD/extra-problems.txt" || :
    fi
    if [ "$os" = NetBSD ]; then
      # NetBSD aarch64 lets the read past the end of a truncated, mapped
      # file through, as DragonFly does, and fails the same two tests:
      #   java.lang.RuntimeException: InternalError not thrown
      #   java.lang.RuntimeException: InternalError was expected
      # NetBSD x86_64 passes both, and its aarch64 fault handler would
      # report the access as SIGBUS (BUS_ADRERR) if it faulted, so no fault
      # is taken at all.  Why is not known.
      cat >> "$PWD/extra-problems.txt" <<'P'
runtime/Unsafe/InternalErrorTest.java 0000000 generic-all
java/foreign/sharedclosejfr/TestSharedCloseJFR.java 0000000 generic-all
P
    fi
    ;;
esac
if [ -s "$PWD/extra-problems.txt" ]; then
  extra="$extra;EXTRA_PROBLEM_LISTS=$PWD/extra-problems.txt"
fi

gmake test-prebuilt $gnu \
  TEST="$suite" \
  BOOT_JDK="$JDK" \
  JT_HOME="$JT" \
  JDK_IMAGE_DIR="$JDK" \
  TEST_IMAGE_DIR="$TESTS" \
  JTREG="JAVA_OPTIONS=-XX:-CreateCoredumpOnCrash;VERBOSE=fail,error,time;KEYWORDS=!headful;TIMEOUT_FACTOR=${TIMEOUT_FACTOR:-4}$extra"

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
    grep -v '^#' | sed 's/[[:space:]].*//' | sort -u | head -40 |
    while read t; do
      [ -n "$t" ] || continue
      # A test with several @test blocks is named Foo.java#id, and jtreg
      # writes each variant to Foo_id.jtr; matching on Foo alone picks up
      # whichever variant find meets first, often not the one that failed.
      f=${t%%#*}
      case "$t" in *#*) f="${f%.*}_${t#*#}" ;; *) f="${f%.*}" ;; esac
      jtr=`find $R/test-support -path "*/$f.jtr" 2>/dev/null | head -1`
      [ -n "$jtr" ] || continue
      echo "--- $t ---"
      grep -E 'Exception|Error|FAILED|failed|expected|timed out|^TEST RESULT' "$jtr" |
        grep -v '^[[:space:]]*at ' | head -15
      # A VM that exits without a word ("Unexpected exit from test [exit
      # code: 1]") leaves nothing for the pattern above; its own output is
      # then the only lead.
      sed -n '/^----------System.err/,/^----------rerun/p' "$jtr" |
        grep -v '^[[:space:]]*at ' | tail -12
    done
  # A crash names only its problematic frame on the console; what faulted
  # and where is in the hs_err file, which otherwise has to be fetched from
  # the artifact.  A gtest crash leaves nothing else to go on.
  find $R/test-support -name 'hs_err_pid*.log' 2>/dev/null | head -4 |
    while read e; do
      echo "--- ${e#$R/test-support/} ---"
      grep -m1 -A1 '^siginfo:' "$e"
      sed -n '/^Registers:/,/^$/p' "$e" | head -24
      sed -n '/^Native frames:/,/^$/p' "$e" | head -16
    done
  echo "--- end ---"
fi
# What the start of the log said about the machine and the split, again at
# the end: only the last 5000 lines of a job's log can be fetched, and a
# long part pushes the start out of reach.
echo "--- setup ---"
uname -srm
cat "$PWD/setup.txt" 2>/dev/null || :
echo "--- end ---"
exit 0
