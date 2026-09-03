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
 */

/*
 * The Serviceability Agent's live-process side for the BSDs other than
 * macOS.  macosx has its own, written against mach_vm_read and
 * task_for_pid; those calls do not exist here, and NetBSD's ptrace(2) is
 * closer to Linux's, so this follows LinuxDebuggerLocal.cpp instead:
 *
 *   PT_ATTACH / PT_DETACH        as on Linux
 *   PT_LWPNEXT                   walks the threads.  Linux reads
 *                                /proc/<pid>/task; there is no such
 *                                directory here, and a ptrace request is
 *                                the documented way
 *   PT_IO with PIOD_READ_D       reads the target's memory in one call.
 *                                Linux uses PTRACE_PEEKDATA a word at a
 *                                time
 *   PT_GETREGS                   fills struct reg, whose members are the
 *                                _REG_* indices of <machine/mcontext.h>
 *
 * Only the live-process case is handled.  Core files go through the
 * shared elf reader, which needs no per-OS code, and are left for later.
 */

#include <jni.h>

#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <machine/reg.h>
#include <machine/mcontext.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal.h"
#include "sun_jvm_hotspot_debugger_amd64_AMD64ThreadContext.h"

#define CHECK_EXCEPTION_(value) if ((*env)->ExceptionOccurred(env)) { return value; }
#define CHECK_EXCEPTION if ((*env)->ExceptionOccurred(env)) { return; }
#define THROW_NEW_DEBUGGER_EXCEPTION_(str, value) { \
    throw_new_debugger_exception(env, str); return value; }
#define THROW_NEW_DEBUGGER_EXCEPTION(str) { \
    throw_new_debugger_exception(env, str); return; }

static jfieldID p_ps_prochandle_ID = 0;
static jfieldID threadList_ID = 0;
static jfieldID loadObjectList_ID = 0;
static jmethodID getThreadForThreadId_ID = 0;
static jmethodID listAdd_ID = 0;

static void throw_new_debugger_exception(JNIEnv* env, const char* errMsg) {
  jclass clazz = (*env)->FindClass(env, "sun/jvm/hotspot/debugger/DebuggerException");
  CHECK_EXCEPTION;
  (*env)->ThrowNew(env, clazz, errMsg);
}

static pid_t get_pid(JNIEnv* env, jobject this_obj) {
  return (pid_t)(intptr_t)(*env)->GetLongField(env, this_obj, p_ps_prochandle_ID);
}

/*
 * Class:     sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal
 * Method:    init0
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal_init0
  (JNIEnv *env, jclass cls) {
  jclass listClass;

  p_ps_prochandle_ID = (*env)->GetFieldID(env, cls, "p_ps_prochandle", "J");
  CHECK_EXCEPTION;
  threadList_ID = (*env)->GetFieldID(env, cls, "threadList", "Ljava/util/List;");
  CHECK_EXCEPTION;
  loadObjectList_ID = (*env)->GetFieldID(env, cls, "loadObjectList", "Ljava/util/List;");
  CHECK_EXCEPTION;
  getThreadForThreadId_ID = (*env)->GetMethodID(env, cls,
      "getThreadForThreadId", "(J)Lsun/jvm/hotspot/debugger/ThreadProxy;");
  CHECK_EXCEPTION;
  listClass = (*env)->FindClass(env, "java/util/List");
  CHECK_EXCEPTION;
  listAdd_ID = (*env)->GetMethodID(env, listClass, "add", "(Ljava/lang/Object;)Z");
  CHECK_EXCEPTION;
}

/*
 * Class:     sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal
 * Method:    getAddressSize
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal_getAddressSize
  (JNIEnv *env, jclass cls) {
  return (jint)(sizeof(uintptr_t) * 8);
}

/*
 * Walk the target's LWPs with PT_LWPNEXT and hand each one to the java
 * side.  Linux enumerates /proc/<pid>/task for this; NetBSD does not
 * mount procfs by default and the request is the documented route.
 */
static void fill_threads(JNIEnv* env, jobject this_obj, pid_t pid) {
  struct ptrace_lwpstatus lwp;
  jobject threadList;

  threadList = (*env)->GetObjectField(env, this_obj, threadList_ID);
  CHECK_EXCEPTION;

  memset(&lwp, 0, sizeof(lwp));
  lwp.pl_lwpid = 0;
  while (ptrace(PT_LWPNEXT, pid, (void*)&lwp, sizeof(lwp)) == 0
         && lwp.pl_lwpid != 0) {
    jobject thread = (*env)->CallObjectMethod(env, this_obj,
        getThreadForThreadId_ID, (jlong)lwp.pl_lwpid);
    CHECK_EXCEPTION;
    (*env)->CallBooleanMethod(env, threadList, listAdd_ID, thread);
    CHECK_EXCEPTION;
    (*env)->DeleteLocalRef(env, thread);
  }
}

/*
 * Class:     sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal
 * Method:    attach0
 * Signature: (I)V
 */
JNIEXPORT void JNICALL Java_sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal_attach0__I
  (JNIEnv *env, jobject this_obj, jint jpid) {
  pid_t pid = (pid_t)jpid;
  int status;

  if (ptrace(PT_ATTACH, pid, NULL, 0) < 0) {
    char msg[256];
    snprintf(msg, sizeof(msg), "ptrace(PT_ATTACH, %d) failed: %s",
             (int)pid, strerror(errno));
    THROW_NEW_DEBUGGER_EXCEPTION(msg);
  }
  /* PT_ATTACH raises SIGSTOP in the target; reap it before going on. */
  if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
    ptrace(PT_DETACH, pid, (void*)1, 0);
    THROW_NEW_DEBUGGER_EXCEPTION("target did not stop after PT_ATTACH");
  }

  (*env)->SetLongField(env, this_obj, p_ps_prochandle_ID, (jlong)(intptr_t)pid);
  fill_threads(env, this_obj, pid);
}

/*
 * Class:     sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal
 * Method:    detach0
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal_detach0
  (JNIEnv *env, jobject this_obj) {
  pid_t pid = get_pid(env, this_obj);
  if (pid != 0) {
    ptrace(PT_DETACH, pid, (void*)1, 0);
    (*env)->SetLongField(env, this_obj, p_ps_prochandle_ID, (jlong)0);
  }
}

/*
 * Class:     sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal
 * Method:    readBytesFromProcess0
 * Signature: (JJ)[B
 */
JNIEXPORT jbyteArray JNICALL Java_sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal_readBytesFromProcess0
  (JNIEnv *env, jobject this_obj, jlong addr, jlong numBytes) {
  pid_t pid = get_pid(env, this_obj);
  jbyteArray array;
  jbyte *buf;
  struct ptrace_io_desc io;

  array = (*env)->NewByteArray(env, (jsize)numBytes);
  CHECK_EXCEPTION_(0);
  buf = (*env)->GetByteArrayElements(env, array, NULL);
  CHECK_EXCEPTION_(0);

  /*
   * PT_IO moves the whole range in one call.  Linux peeks a word at a
   * time because PTRACE_PEEKDATA is all it has.
   */
  io.piod_op = PIOD_READ_D;
  io.piod_offs = (void*)(uintptr_t)addr;
  io.piod_addr = buf;
  io.piod_len = (size_t)numBytes;

  if (ptrace(PT_IO, pid, (void*)&io, 0) != 0 || io.piod_len != (size_t)numBytes) {
    (*env)->ReleaseByteArrayElements(env, array, buf, JNI_ABORT);
    return NULL;
  }
  (*env)->ReleaseByteArrayElements(env, array, buf, 0);
  return array;
}

/*
 * Class:     sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal
 * Method:    getThreadIntegerRegisterSet0
 * Signature: (J)[J
 */
JNIEXPORT jlongArray JNICALL Java_sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal_getThreadIntegerRegisterSet0
  (JNIEnv *env, jobject this_obj, jlong thread_id) {
  pid_t pid = get_pid(env, this_obj);
  struct reg gregs;
  jlongArray array;
  jlong *regs;

  if (ptrace(PT_GETREGS, pid, (void*)&gregs, (int)thread_id) != 0) {
    /*
     * Not fatal: the same is true on Linux, where an ESRCH here makes the
     * stack walker fall back on the last java frame.
     */
    return NULL;
  }

#define NPRGREG   sun_jvm_hotspot_debugger_amd64_AMD64ThreadContext_NPRGREG
#define REG_INDEX(reg) sun_jvm_hotspot_debugger_amd64_AMD64ThreadContext_##reg

  array = (*env)->NewLongArray(env, NPRGREG);
  CHECK_EXCEPTION_(0);
  regs = (*env)->GetLongArrayElements(env, array, NULL);
  CHECK_EXCEPTION_(0);

  /* struct reg is __gregset_t, indexed by the _REG_* enum. */
  regs[REG_INDEX(R15)]    = gregs.regs[_REG_R15];
  regs[REG_INDEX(R14)]    = gregs.regs[_REG_R14];
  regs[REG_INDEX(R13)]    = gregs.regs[_REG_R13];
  regs[REG_INDEX(R12)]    = gregs.regs[_REG_R12];
  regs[REG_INDEX(R11)]    = gregs.regs[_REG_R11];
  regs[REG_INDEX(R10)]    = gregs.regs[_REG_R10];
  regs[REG_INDEX(R9)]     = gregs.regs[_REG_R9];
  regs[REG_INDEX(R8)]     = gregs.regs[_REG_R8];
  regs[REG_INDEX(RDI)]    = gregs.regs[_REG_RDI];
  regs[REG_INDEX(RSI)]    = gregs.regs[_REG_RSI];
  regs[REG_INDEX(RBP)]    = gregs.regs[_REG_RBP];
  regs[REG_INDEX(RBX)]    = gregs.regs[_REG_RBX];
  regs[REG_INDEX(RDX)]    = gregs.regs[_REG_RDX];
  regs[REG_INDEX(RCX)]    = gregs.regs[_REG_RCX];
  regs[REG_INDEX(RAX)]    = gregs.regs[_REG_RAX];
  regs[REG_INDEX(TRAPNO)] = gregs.regs[_REG_TRAPNO];
  regs[REG_INDEX(ERR)]    = gregs.regs[_REG_ERR];
  regs[REG_INDEX(RIP)]    = gregs.regs[_REG_RIP];
  regs[REG_INDEX(CS)]     = gregs.regs[_REG_CS];
  regs[REG_INDEX(RFL)]    = gregs.regs[_REG_RFLAGS];
  regs[REG_INDEX(RSP)]    = gregs.regs[_REG_RSP];
  regs[REG_INDEX(SS)]     = gregs.regs[_REG_SS];
  regs[REG_INDEX(FS)]     = gregs.regs[_REG_FS];
  regs[REG_INDEX(GS)]     = gregs.regs[_REG_GS];
  regs[REG_INDEX(ES)]     = gregs.regs[_REG_ES];
  regs[REG_INDEX(DS)]     = gregs.regs[_REG_DS];
  /* NetBSD's struct reg carries no FS_BASE or GS_BASE. */
  regs[REG_INDEX(FSBASE)] = 0;
  regs[REG_INDEX(GSBASE)] = 0;

  (*env)->ReleaseLongArrayElements(env, array, regs, 0);
  return array;
}
