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

#ifndef OS_CPU_BSD_PPC_UCONTEXT_BSD_PPC_HPP
#define OS_CPU_BSD_PPC_UCONTEXT_BSD_PPC_HPP

// Where each BSD keeps the registers a signal handler was handed, spelled
// as members of a ucontext_t: uc->context_pc, uc->context_gpr(1) and so on.
//
// Unlike Linux, which reaches the volatile registers through a pointer the
// kernel may or may not have filled in, the BSDs keep them in the context
// itself, so there is nothing to check for before reading one.  OpenBSD
// has no ucontext at all: its ucontext_t is struct sigcontext.
//
// Only for .cpp files: the names are too short to leak into every
// translation unit that includes a header.

#include <signal.h>
#ifndef __OpenBSD__
#include <ucontext.h>
#endif

#if defined(__FreeBSD__)
// <machine/ucontext.h>: mc_gpr, mc_lr, mc_ctr and mc_srr0 name slots of
// mc_frame.
# define context_gpr(n) uc_mcontext.mc_gpr[n]
# define context_pc     uc_mcontext.mc_srr0
# define context_lr     uc_mcontext.mc_lr
# define context_ctr    uc_mcontext.mc_ctr
#elif defined(__NetBSD__)
// <powerpc/mcontext.h>: __gregs indexed by _REG_R0.._REG_R31, _REG_PC, ...
# define context_gpr(n) uc_mcontext.__gregs[_REG_R0 + (n)]
# define context_pc     uc_mcontext.__gregs[_REG_PC]
# define context_lr     uc_mcontext.__gregs[_REG_LR]
# define context_ctr    uc_mcontext.__gregs[_REG_CTR]
#elif defined(__OpenBSD__)
// <powerpc64/signal.h>: struct sigcontext.
# define context_gpr(n) sc_reg[n]
# define context_pc     sc_pc
# define context_lr     sc_lr
# define context_ctr    sc_ctr
#else
# error "Say where this BSD keeps the PowerPC registers of a signal context"
#endif

#endif // OS_CPU_BSD_PPC_UCONTEXT_BSD_PPC_HPP
