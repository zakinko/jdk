/*
 * Copyright (c) 1997, 2026, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012, 2026 SAP SE. All rights reserved.
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

#include "assembler_ppc.hpp"
#include "asm/assembler.inline.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/codeCache.hpp"
#include "code/vtableStubs.hpp"
#include "cppstdlib/cstdlib.hpp"
#include "interpreter/interpreter.hpp"
#include "jvm.h"
#include "memory/allocation.inline.hpp"
#include "nativeInst_ppc.hpp"
#include "os_bsd.hpp"
#include "os_posix.hpp"
#include "prims/jniFastGetField.hpp"
#include "prims/jvm_misc.hpp"
#include "runtime/arguments.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.inline.hpp"
#include "runtime/osThread.hpp"
#include "runtime/safepointMechanism.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/timer.hpp"
#include "runtime/vm_version.hpp"
#include "signals_posix.hpp"
#include "ucontext_bsd_ppc.hpp"
#include "utilities/debug.hpp"
#include "utilities/events.hpp"
#include "utilities/vmError.hpp"

// put OS-includes here
# include <sys/types.h>
# include <sys/mman.h>
# include <pthread.h>
# include <signal.h>
# include <errno.h>
# include <dlfcn.h>
# include <stdio.h>
# include <unistd.h>
# include <sys/resource.h>
# include <sys/stat.h>
# include <sys/time.h>
# include <sys/utsname.h>
# include <sys/socket.h>
# include <sys/wait.h>
# include <pwd.h>
# include <poll.h>
#ifndef __OpenBSD__
# include <ucontext.h>
#endif
#ifdef __FreeBSD__
# include <sys/sysctl.h>
# include <sys/procctl.h>
# ifndef PROC_STACKGAP_STATUS
#  define PROC_STACKGAP_STATUS 18
# endif
# ifndef PROC_STACKGAP_DISABLE
#  define PROC_STACKGAP_DISABLE 0x0002
# endif
#endif /* __FreeBSD__ */

address os::current_stack_pointer() {
  return (address)__builtin_frame_address(0);
}

char* os::non_memory_address_word() {
  // Must never look like an address returned by reserve_memory,
  // even in its subfields (as defined by the CPU immediate fields,
  // if the CPU splits constants across multiple instructions).

  return (char*) -1;
}

// Frame information (pc, sp, fp) retrieved via ucontext
// always looks like a C-frame according to the frame
// conventions in frame_ppc64.hpp.
// Unlike Linux, which reaches the volatile registers through a pointer the
// kernel may or may not have filled in, the BSDs keep them in the context
// itself, so there is nothing to check for before reading one.
address os::Posix::ucontext_get_pc(const ucontext_t * uc) {
  return (address)uc->context_pc;
}

// modify PC in ucontext.
// Note: Only use this for an ucontext handed down to a signal handler.
void os::Posix::ucontext_set_pc(ucontext_t * uc, address pc) {
  uc->context_pc = (intptr_t)pc;
}

static address ucontext_get_lr(const ucontext_t * uc) {
  return (address)uc->context_lr;
}

intptr_t* os::Bsd::ucontext_get_sp(const ucontext_t * uc) {
  return (intptr_t*)uc->context_gpr(1/*REG_SP*/);
}

intptr_t* os::Bsd::ucontext_get_fp(const ucontext_t * uc) {
  return nullptr;
}

address os::fetch_frame_from_context(const void* ucVoid,
                    intptr_t** ret_sp, intptr_t** ret_fp) {

  address epc;
  const ucontext_t* uc = (const ucontext_t*)ucVoid;

  if (uc != nullptr) {
    epc = os::Posix::ucontext_get_pc(uc);
    if (ret_sp) *ret_sp = os::Bsd::ucontext_get_sp(uc);
    if (ret_fp) *ret_fp = os::Bsd::ucontext_get_fp(uc);
  } else {
    epc = nullptr;
    if (ret_sp) *ret_sp = (intptr_t *)nullptr;
    if (ret_fp) *ret_fp = (intptr_t *)nullptr;
  }

  return epc;
}

frame os::fetch_frame_from_context(const void* ucVoid) {
  intptr_t* sp;
  intptr_t* fp;
  address epc = fetch_frame_from_context(ucVoid, &sp, &fp);
  if (!is_readable_pointer(epc)) {
    // Try to recover from calling into bad memory
    // Assume new frame has not been set up, the same as
    // compiled frame stack bang
    return fetch_compiled_frame_from_context(ucVoid);
  }
  return frame(sp, epc, frame::kind::unknown);
}

frame os::fetch_compiled_frame_from_context(const void* ucVoid) {
  const ucontext_t* uc = (const ucontext_t*)ucVoid;
  intptr_t* sp = os::Bsd::ucontext_get_sp(uc);
  address lr = ucontext_get_lr(uc);
  return frame(sp, lr, frame::kind::unknown);
}

intptr_t* os::fetch_bcp_from_context(const void* ucVoid) {
  assert(ucVoid != nullptr, "invariant");
  const ucontext_t* uc = (const ucontext_t*)ucVoid;
  assert(os::Posix::ucontext_is_interpreter(uc), "invariant");
  return reinterpret_cast<intptr_t*>(uc->context_gpr(14)); // R14_bcp
}

frame os::get_sender_for_C_frame(frame* fr) {
  if (*fr->sp() == 0) {
    // fr is the last C frame
    return frame();
  }
  return frame(fr->sender_sp(), fr->sender_pc(), frame::kind::unknown);
}


frame os::current_frame() {
  intptr_t* csp = *(intptr_t**) __builtin_frame_address(0);
  frame topframe(csp, CAST_FROM_FN_PTR(address, os::current_frame), frame::kind::unknown);
  return os::get_sender_for_C_frame(&topframe);
}

bool PosixSignals::pd_hotspot_signal_handler(int sig, siginfo_t* info,
                                             ucontext_t* uc, JavaThread* thread) {

  // decide if this trap can be handled by a stub
  address stub = nullptr;
  address pc   = nullptr;

  if (info != nullptr && uc != nullptr && thread != nullptr) {
    pc = (address) os::Posix::ucontext_get_pc(uc);

    // Handle ALL stack overflow variations here
    if (sig == SIGSEGV) {
      // Use get_stack_bang_address instead of si_addr, as on Linux.
      // If SIGSEGV is caused due to a branch to an invalid address an
      // "Instruction Storage Interrupt" is generated and 'pc' (NIP) already
      // contains the invalid address. Otherwise, the SIGSEGV is caused due to
      // load/store instruction trying to load/store from/to an invalid address
      // and causing a "Data Storage Interrupt", so we inspect the instruction
      // in order to extract the faulty data address.
      // Linux tells the two apart by the trap number in the mcontext, which
      // not every BSD saves; the fault address says the same, since only an
      // instruction fetch faults at the pc itself.
      address addr;
      if ((address)info->si_addr == pc) {
        // Instruction Storage Interrupt (ISI)
        addr = pc;
      } else {
        // Data Storage Interrupt (DSI): extract faulty data address
        addr = ((NativeInstruction*)pc)->get_stack_bang_address(uc);
      }

#ifdef __FreeBSD__
      /*
       * Determine whether the kernel stack guard pages have been disabled.
       * Only a stack bang needs to know, and asking costs a system call on
       * every SIGSEGV, implicit null checks among them.
       */
      int status = 0;
      int ret = (addr == nullptr) ? 0 :
                procctl(P_PID, getpid(), PROC_STACKGAP_STATUS, &status);

      /*
       * Check if the call to procctl(2) failed or the stack guard is not
       * disabled.  Either way, we'll then attempt a workaround.
       */
      if (addr != nullptr && (ret == -1 || !(status & PROC_STACKGAP_DISABLE))) {
          /*
           * Try to work around the problems caused on FreeBSD where the kernel
           * may place guard pages above JVM guard pages and prevent the Java
           * thread stacks growing into the JVM guard pages.  The work around
           * is to determine how many such pages there may be and round down the
           * fault address so that tests of whether it is in the JVM guard zone
           * succeed.
           *
           * Note that this is a partial workaround at best since the normally
           * the JVM could then unprotect the reserved area to allow a critical
           * section to complete.  This is not possible if the kernel has
           * placed guard pages below the reserved area.
           *
           * This also suffers from the problem that the
           * security.bsd.stack_guard_page sysctl is dynamic and may have
           * changed since the stack was allocated.  This is likely to be rare
           * in practice though.
           *
           * What this does do is prevent the JVM crashing on FreeBSD and
           * instead throwing a StackOverflowError when infinite recursion
           * is attempted, which is the expected behaviour.  Due to it's
           * limitations though, objects may be in unexpected states when
           * this occurs.
           *
           * A better way to avoid these problems is either to be on a new
           * enough version of FreeBSD (one that has PROC_STACKGAP_CTL) or set
           * security.bsd.stack_guard_page to zero.
           */
          int guard_pages = 0;
          size_t size = sizeof(guard_pages);
          if (sysctlbyname("security.bsd.stack_guard_page",
                           &guard_pages, &size, nullptr, 0) == 0 &&
              guard_pages > 0) {
            addr -= guard_pages * os::vm_page_size();
          }
      }
#endif

      // Check if fault address is within thread stack.
      if (thread->is_in_full_stack(addr)) {
        // stack overflow
        if (os::Posix::handle_stack_overflow(thread, addr, pc, uc, &stub)) {
          return true; // continue
        }
      }
    }

    if (thread->thread_state() == _thread_in_Java) {
      // Java thread running in Java code => find exception handler if any
      // a fault inside compiled code, the interpreter, or a stub

      CodeBlob *cb = nullptr;
      int stop_type = -1;
      if ((sig == (USE_POLL_BIT_ONLY ? SIGTRAP : SIGSEGV)) &&
               // A linux-ppc64 kernel before 2.6.6 doesn't set si_addr on some segfaults
               // in 64bit mode (cf. http://www.kernel.org/pub/linux/kernel/v2.6/ChangeLog-2.6.6),
               // especially when we try to read from the safepoint polling page. So the check
               //   (address)info->si_addr == os::get_standard_polling_page()
               // doesn't work for us. We use:
               ((NativeInstruction*)pc)->is_safepoint_poll() &&
               CodeCache::contains((void*) pc) &&
               ((cb = CodeCache::find_blob(pc)) != nullptr) &&
               cb->is_nmethod()) {
        if (TraceTraps) {
          tty->print_cr("trap: safepoint_poll at " INTPTR_FORMAT " (%s)", p2i(pc),
                        USE_POLL_BIT_ONLY ? "SIGTRAP" : "SIGSEGV");
        }
        stub = SharedRuntime::get_poll_stub(pc);
      }

      else if (UseSIGTRAP && sig == SIGTRAP &&
               ((NativeInstruction*)pc)->is_safepoint_poll_return() &&
               CodeCache::contains((void*) pc) &&
               ((cb = CodeCache::find_blob(pc)) != nullptr) &&
               cb->is_nmethod()) {
        if (TraceTraps) {
          tty->print_cr("trap: safepoint_poll at return at " INTPTR_FORMAT " (nmethod)", p2i(pc));
        }
        stub = SharedRuntime::polling_page_return_handler_blob()->entry_point();
      }

      // SIGTRAP-based nmethod entry barriers.
      else if (sig == SIGTRAP && TrapBasedNMethodEntryBarriers &&
               nativeInstruction_at(pc)->is_sigtrap_nmethod_entry_barrier() &&
               CodeCache::contains((void*) pc)) {
        if (TraceTraps) {
          tty->print_cr("trap: nmethod entry barrier at " INTPTR_FORMAT " (SIGTRAP)", p2i(pc));
        }
        stub = StubRoutines::method_entry_barrier();
        uc->context_lr = (intptr_t)(pc + BytesPerInstWord); // emulate call by setting LR
      }

      // SIGTRAP-based ic miss check in compiled code.
      else if (sig == SIGTRAP && TrapBasedICMissChecks &&
               nativeInstruction_at(pc)->is_sigtrap_ic_miss_check()) {
        if (TraceTraps) {
          tty->print_cr("trap: ic_miss_check at " INTPTR_FORMAT " (SIGTRAP)", p2i(pc));
        }
        stub = SharedRuntime::get_ic_miss_stub();
      }

      // SIGTRAP-based implicit null check in compiled code.
      else if (sig == SIGTRAP && TrapBasedNullChecks &&
               nativeInstruction_at(pc)->is_sigtrap_null_check()) {
        if (TraceTraps) {
          tty->print_cr("trap: null_check at " INTPTR_FORMAT " (SIGTRAP)", p2i(pc));
        }
        stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::IMPLICIT_NULL);
      }

      // SIGSEGV-based implicit null check in compiled code.
      else if (sig == SIGSEGV && ImplicitNullChecks &&
               CodeCache::contains((void*) pc) &&
               MacroAssembler::uses_implicit_null_check(info->si_addr)) {
        if (TraceTraps) {
          tty->print_cr("trap: null_check at " INTPTR_FORMAT " (SIGSEGV)", p2i(pc));
        }
        stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::IMPLICIT_NULL);
      }

#ifdef COMPILER2
      // SIGTRAP-based implicit range check in compiled code.
      else if (sig == SIGTRAP && TrapBasedRangeChecks &&
               nativeInstruction_at(pc)->is_sigtrap_range_check()) {
        if (TraceTraps) {
          tty->print_cr("trap: range_check at " INTPTR_FORMAT " (SIGTRAP)", p2i(pc));
        }
        stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::IMPLICIT_NULL);
      }
#endif

      // stop on request
      else if (sig == SIGTRAP && (stop_type = nativeInstruction_at(pc)->get_stop_type()) != -1) {
        bool msg_present = (stop_type & MacroAssembler::stop_msg_present);
        stop_type = (stop_type &~ MacroAssembler::stop_msg_present);

        const char *msg = nullptr;
        switch (stop_type) {
          case MacroAssembler::stop_stop              : msg = "stop"; break;
          case MacroAssembler::stop_untested          : msg = "untested"; break;
          case MacroAssembler::stop_unimplemented     : msg = "unimplemented"; break;
          case MacroAssembler::stop_shouldnotreachhere: msg = "shouldnotreachhere"; break;
          default: msg = "unknown"; break;
        }

        const char **detail_msg_ptr = (const char**)(pc + 4);
        const char *detail_msg = msg_present ? *detail_msg_ptr : "no details provided";

        if (TraceTraps) {
          tty->print_cr("trap: %s: %s (SIGTRAP, stop type %d)", msg, detail_msg, stop_type);
        }

        // End life with a fatal error, message and detail message and the context.
        // Note: no need to do any post-processing here (e.g. signal chaining)
        VMError::report_and_die(thread, uc, nullptr, 0, msg, "%s", detail_msg);

        ShouldNotReachHere();

      }

      else if (sig == SIGBUS) {
        // BugId 4454115: A read from a MappedByteBuffer can fault here if the
        // underlying file has been truncated. Do not crash the VM in such a case.
        CodeBlob* cb = CodeCache::find_blob(pc);
        nmethod* nm = (cb != nullptr) ? cb->as_nmethod_or_null() : nullptr;
        bool is_unsafe_memory_access = (thread->doing_unsafe_access() && UnsafeMemoryAccess::contains_pc(pc));
        if ((nm != nullptr && nm->has_unsafe_access()) || is_unsafe_memory_access) {
          address next_pc = pc + 4;
          if (is_unsafe_memory_access) {
            next_pc = UnsafeMemoryAccess::page_error_continue_pc(pc);
          }
          next_pc = SharedRuntime::handle_unsafe_access(thread, next_pc);
          os::Posix::ucontext_set_pc(uc, next_pc);
          return true;
        }
      }
    }

    else { // thread->thread_state() != _thread_in_Java
      if (sig == SIGILL && VM_Version::is_determine_features_test_running()) {
        // SIGILL must be caused by VM_Version::determine_features().
        *(int *)pc = 0; // patch instruction to 0 to indicate that it causes a SIGILL,
                        // flushing of icache is not necessary.
        stub = pc + 4;  // continue with next instruction.
      }
      else if ((thread->thread_state() == _thread_in_vm ||
                thread->thread_state() == _thread_in_native) &&
               sig == SIGBUS && thread->doing_unsafe_access()) {
        address next_pc = pc + 4;
        if (UnsafeMemoryAccess::contains_pc(pc)) {
          next_pc = UnsafeMemoryAccess::page_error_continue_pc(pc);
        }
        next_pc = SharedRuntime::handle_unsafe_access(thread, next_pc);
        os::Posix::ucontext_set_pc(uc, next_pc);
        return true;
      }
    }

    // jni_fast_Get<Primitive>Field can trap at certain pc's if a GC kicks in
    // and the heap gets shrunk before the field access.
    if (stub == nullptr && ((sig == SIGSEGV) || (sig == SIGBUS))) {
      address addr = JNI_FastGetField::find_slowcase_pc(pc);
      if (addr != (address)-1) {
        stub = addr;
      }
    }
  }

  if (stub != nullptr) {
    // Save all thread context in case we need to restore it.
    if (thread != nullptr) thread->set_saved_exception_pc(pc);
    os::Posix::ucontext_set_pc(uc, stub);
    return true;
  }

  return false;
}

void os::Bsd::init_thread_fpu_state(void) {
  // Disable FP exceptions.
  __asm__ __volatile__ ("mtfsfi 6,0");
}

////////////////////////////////////////////////////////////////////////////////
// thread stack

// Minimum usable stack sizes required to get to user code. Space for
// HotSpot guard pages is added later.
size_t os::_compiler_thread_min_stack_allowed = 64 * K;
size_t os::_java_thread_min_stack_allowed = 64 * K;
size_t os::_vm_internal_thread_min_stack_allowed = 64 * K;

// Return default stack size for thr_type.
size_t os::Posix::default_stack_size(os::ThreadType thr_type) {
  // Default stack size (compiler thread needs larger stack).
  size_t s = (thr_type == os::compiler_thread ? 4 * M : 1024 * K);
  return s;
}

/////////////////////////////////////////////////////////////////////////////
// helper functions for fatal error handler

void os::print_context(outputStream *st, const void *context) {
  if (context == nullptr) return;

  const ucontext_t* uc = (const ucontext_t*)context;

  st->print_cr("Registers:");
  st->print("pc =" INTPTR_FORMAT "  ", (intptr_t)uc->context_pc);
  st->print("lr =" INTPTR_FORMAT "  ", (intptr_t)uc->context_lr);
  st->print("ctr=" INTPTR_FORMAT "  ", (intptr_t)uc->context_ctr);
  st->cr();
  for (int i = 0; i < 32; i++) {
    st->print("r%-2d=" INTPTR_FORMAT "  ", i, (intptr_t)uc->context_gpr(i));
    if (i % 3 == 2) st->cr();
  }
  st->cr();
  st->cr();
}

void os::print_register_info(outputStream *st, const void *context, int& continuation) {
  const int register_count = 32 /* r0-r31 */ + 3 /* pc, lr, ctr */;
  int n = continuation;
  assert(n >= 0 && n <= register_count, "Invalid continuation value");
  if (context == nullptr || n == register_count) {
    return;
  }

  const ucontext_t *uc = (const ucontext_t*)context;
  while (n < register_count) {
    // Update continuation with next index before printing location
    continuation = n + 1;
    switch (n) {
    case 0:
      st->print("pc ="); print_location(st, (intptr_t)uc->context_pc);
      break;
    case 1:
      st->print("lr ="); print_location(st, (intptr_t)uc->context_lr);
      break;
    case 2:
      st->print("ctr ="); print_location(st, (intptr_t)uc->context_ctr);
      break;
    default:
      st->print("r%-2d=", n-3);
      print_location(st, (intptr_t)uc->context_gpr(n-3));
      break;
    }
    ++n;
  }
}

extern "C" {
  int SpinPause() {
    // Setting prio low, then prio medium results in a pseudo yield.
    // Yield (or 27,27,27) was never implemented on PPC.
    //   or 1,1,1 = smt_prio_low
    //   or 2,2,2 = smt_prio_medium
    asm volatile ("or 1,1,1\n\t"
                  "or 2,2,2" : : : "memory");
    return 1;
  }
}

#ifndef PRODUCT
void os::verify_stack_alignment() {
  assert(((intptr_t)os::current_stack_pointer() & (StackAlignmentInBytes-1)) == 0, "incorrect stack alignment");
}
#endif

int os::extra_bang_size_in_bytes() {
  // PPC does not require the additional stack bang.
  return 0;
}

void os::setup_fpu() {}
