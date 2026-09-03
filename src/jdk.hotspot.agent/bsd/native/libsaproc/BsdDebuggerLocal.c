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
 * Symbols are a separate problem.  Linux reads /proc/<pid>/maps to learn
 * what is mapped where; there is no such file here, so the mappings come
 * from kinfo_getvmmap(3), which libutil provides on NetBSD and FreeBSD
 * alike and which reports the same path-and-range triples.  The symbol
 * tables themselves are then read out of the files on disk rather than
 * out of the target, exactly as Linux's symtab.c does: the tables are not
 * loaded at run time, so the target has no copy to read.
 *
 * Only the live-process case is handled.  Core files go through the
 * shared elf reader, which needs no per-OS code, and are left for later.
 */

#include <jni.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <machine/reg.h>
#include <machine/mcontext.h>

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __FreeBSD__
#include <libutil.h>
#else
#include <util.h>
#endif

#include "sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal.h"
#include "sun_jvm_hotspot_debugger_amd64_AMD64ThreadContext.h"

#define CHECK_EXCEPTION_(value) if ((*env)->ExceptionOccurred(env)) { return value; }
#define CHECK_EXCEPTION if ((*env)->ExceptionOccurred(env)) { return; }
#define THROW_NEW_DEBUGGER_EXCEPTION_(str, value) { \
    throw_new_debugger_exception(env, str); return value; }
#define THROW_NEW_DEBUGGER_EXCEPTION(str) { \
    throw_new_debugger_exception(env, str); return; }

static jfieldID p_ps_prochandle_ID = 0;
static jfieldID loadObjectList_ID = 0;
static jmethodID createClosestSymbol_ID = 0;
static jmethodID createLoadObject_ID = 0;
static jmethodID listAdd_ID = 0;

static void throw_new_debugger_exception(JNIEnv* env, const char* errMsg) {
  jclass clazz = (*env)->FindClass(env, "sun/jvm/hotspot/debugger/DebuggerException");
  CHECK_EXCEPTION;
  (*env)->ThrowNew(env, clazz, errMsg);
}

/*
 * One file mapped into the target.  base is the address the file's own
 * vaddr 0 would sit at, so a link-time value from its symbol table
 * becomes a runtime one by adding base and taking off the lowest vaddr
 * the file's PT_LOAD headers name.
 */
typedef struct lib_info {
  char             *name;
  uintptr_t         base;
  uintptr_t         end;
  struct lib_info  *next;
} lib_info;

struct ps_prochandle {
  pid_t     pid;
  lib_info *libs;
};

static struct ps_prochandle* get_proc(JNIEnv* env, jobject this_obj) {
  return (struct ps_prochandle*)(intptr_t)
      (*env)->GetLongField(env, this_obj, p_ps_prochandle_ID);
}

static pid_t get_pid(JNIEnv* env, jobject this_obj) {
  struct ps_prochandle* ph = get_proc(env, this_obj);
  return ph == NULL ? 0 : ph->pid;
}

static const char* base_name(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash == NULL ? path : slash + 1;
}

/* ---------------------------------------------------------------- ELF */

static int map_file(const char* path, char** addr, size_t* size) {
  struct stat st;
  void* p;
  int fd = open(path, O_RDONLY);

  if (fd < 0) {
    return -1;
  }
  if (fstat(fd, &st) < 0 || (size_t)st.st_size < sizeof(Elf64_Ehdr)) {
    close(fd);
    return -1;
  }
  p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    return -1;
  }
  *addr = (char*)p;
  *size = (size_t)st.st_size;
  return 0;
}

/* Rejects anything that is not an ELF of this machine's own class. */
static const Elf64_Ehdr* elf_header(const char* addr, size_t size) {
  const Elf64_Ehdr* eh = (const Elf64_Ehdr*)addr;

  if (size < sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
    return NULL;
  }
  if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
    return NULL;
  }
  if (eh->e_shoff == 0 ||
      eh->e_shoff + (size_t)eh->e_shnum * eh->e_shentsize > size) {
    return NULL;
  }
  return eh;
}

/* Lowest vaddr any PT_LOAD asks for; 0 for the shared objects. */
static uintptr_t elf_vaddr0(const char* addr, size_t size,
                            const Elf64_Ehdr* eh) {
  uintptr_t lowest = (uintptr_t)-1;
  Elf64_Half i;

  if (eh->e_phoff + (size_t)eh->e_phnum * eh->e_phentsize > size) {
    return 0;
  }
  for (i = 0; i < eh->e_phnum; i++) {
    const Elf64_Phdr* ph =
        (const Elf64_Phdr*)(addr + eh->e_phoff + (size_t)i * eh->e_phentsize);
    if (ph->p_type == PT_LOAD && (uintptr_t)ph->p_vaddr < lowest) {
      lowest = (uintptr_t)ph->p_vaddr;
    }
  }
  return lowest == (uintptr_t)-1 ? 0 : lowest;
}

/*
 * Both .symtab and .dynsym are walked.  A stripped libjvm.so keeps only
 * the latter, and the agent asks for names -- gHotSpotVMStructs and the
 * rest -- that are exported either way; searching both means one code
 * path serves a product build and a debug one.
 */
typedef int (*sym_visitor)(const char* name, const Elf64_Sym* sym, void* arg);

static void elf_walk_symbols(const char* addr, size_t size,
                             const Elf64_Ehdr* eh,
                             sym_visitor visit, void* arg) {
  Elf64_Half i;

  for (i = 0; i < eh->e_shnum; i++) {
    const Elf64_Shdr* sh =
        (const Elf64_Shdr*)(addr + eh->e_shoff + (size_t)i * eh->e_shentsize);
    const Elf64_Shdr* strh;
    const char* strtab;
    size_t n, j;

    if (sh->sh_type != SHT_SYMTAB && sh->sh_type != SHT_DYNSYM) {
      continue;
    }
    if (sh->sh_link >= eh->e_shnum || sh->sh_entsize == 0) {
      continue;
    }
    if (sh->sh_offset + sh->sh_size > size) {
      continue;
    }
    strh = (const Elf64_Shdr*)(addr + eh->e_shoff +
                               (size_t)sh->sh_link * eh->e_shentsize);
    if (strh->sh_offset + strh->sh_size > size) {
      continue;
    }
    strtab = addr + strh->sh_offset;
    n = (size_t)(sh->sh_size / sh->sh_entsize);
    for (j = 0; j < n; j++) {
      const Elf64_Sym* sym =
          (const Elf64_Sym*)(addr + sh->sh_offset + j * sh->sh_entsize);
      if (sym->st_name == 0 || sym->st_name >= strh->sh_size) {
        continue;
      }
      if (visit(strtab + sym->st_name, sym, arg) != 0) {
        return;
      }
    }
  }
}

struct find_name {
  const char* want;
  uintptr_t   value;
  int         found;
};

static int visit_name(const char* name, const Elf64_Sym* sym, void* arg) {
  struct find_name* f = (struct find_name*)arg;

  if (sym->st_shndx == SHN_UNDEF || strcmp(name, f->want) != 0) {
    return 0;
  }
  f->value = (uintptr_t)sym->st_value;
  f->found = 1;
  return 1;
}

struct find_addr {
  uintptr_t target;    /* link-time vaddr being looked for */
  uintptr_t best;
  char      name[256];
  int       found;
};

static int visit_addr(const char* name, const Elf64_Sym* sym, void* arg) {
  struct find_addr* f = (struct find_addr*)arg;
  unsigned char type = ELF64_ST_TYPE(sym->st_info);

  if (sym->st_shndx == SHN_UNDEF || sym->st_value == 0) {
    return 0;
  }
  if (type != STT_FUNC && type != STT_OBJECT) {
    return 0;
  }
  if ((uintptr_t)sym->st_value > f->target) {
    return 0;
  }
  /*
   * A symbol whose size is known and which the address falls past is not
   * the answer, however close it is; the address belongs to whatever
   * comes after, or to no symbol at all.
   */
  if (sym->st_size != 0 &&
      f->target >= (uintptr_t)sym->st_value + (uintptr_t)sym->st_size) {
    return 0;
  }
  if (!f->found || (uintptr_t)sym->st_value > f->best) {
    f->best = (uintptr_t)sym->st_value;
    f->found = 1;
    snprintf(f->name, sizeof(f->name), "%s", name);
  }
  return 0;
}

/* --------------------------------------------------------- load objects */

/* Runtime address of sym in lib, or 0 if lib does not define it. */
static jlong lookup_in_lib(lib_info* lib, const char* sym) {
  char* addr;
  size_t size;
  const Elf64_Ehdr* eh;
  struct find_name f;
  jlong result = 0;

  if (map_file(lib->name, &addr, &size) != 0) {
    return 0;
  }
  if ((eh = elf_header(addr, size)) != NULL) {
    memset(&f, 0, sizeof(f));
    f.want = sym;
    elf_walk_symbols(addr, size, eh, visit_name, &f);
    if (f.found) {
      result = (jlong)(lib->base - elf_vaddr0(addr, size, eh) + f.value);
    }
  }
  munmap(addr, size);
  return result;
}

static void free_libs(lib_info* lib) {
  while (lib != NULL) {
    lib_info* next = lib->next;
    free(lib->name);
    free(lib);
    lib = next;
  }
}

static lib_info* find_lib(struct ps_prochandle* ph, const char* objectName) {
  lib_info* lib;

  for (lib = ph->libs; lib != NULL; lib = lib->next) {
    if (objectName == NULL || strcmp(lib->name, objectName) == 0 ||
        strcmp(base_name(lib->name), base_name(objectName)) == 0) {
      return lib;
    }
  }
  return NULL;
}

static void add_mapping(struct ps_prochandle* ph, const char* path,
                        uintptr_t start, uintptr_t end, uintptr_t offset) {
  lib_info* lib;

  for (lib = ph->libs; lib != NULL; lib = lib->next) {
    if (strcmp(lib->name, path) == 0) {
      /* A file arrives in several pieces; keep the outermost of them. */
      if (start - offset < lib->base) {
        lib->base = start - offset;
      }
      if (end > lib->end) {
        lib->end = end;
      }
      return;
    }
  }
  lib = (lib_info*)calloc(1, sizeof(*lib));
  if (lib == NULL) {
    return;
  }
  lib->name = strdup(path);
  if (lib->name == NULL) {
    free(lib);
    return;
  }
  lib->base = start - offset;
  lib->end = end;
  lib->next = ph->libs;
  ph->libs = lib;
}

/*
 * The kernel names every mapping, including the ones backed by files that
 * are not objects at all -- a font, a jar opened with mmap.  Those are
 * dropped here rather than at lookup time so that the load object list
 * the agent hands back holds only things it can make sense of.
 */
static int is_elf_file(const char* path) {
  char magic[SELFMAG];
  int n;
  int fd = open(path, O_RDONLY);

  if (fd < 0) {
    return 0;
  }
  n = (int)read(fd, magic, sizeof(magic));
  close(fd);
  return n == (int)sizeof(magic) && memcmp(magic, ELFMAG, SELFMAG) == 0;
}

static void fill_load_objects(JNIEnv* env, jobject this_obj,
                              struct ps_prochandle* ph) {
  struct kinfo_vmentry* vmmap;
  lib_info* lib;
  jobject list;
#ifdef __FreeBSD__
  int nent = 0;
#else
  size_t nent = 0;
#endif
  size_t i;

  vmmap = kinfo_getvmmap(ph->pid, &nent);
  if (vmmap == NULL) {
    return;
  }
  for (i = 0; i < (size_t)nent; i++) {
    if (vmmap[i].kve_path[0] != '/' || !is_elf_file(vmmap[i].kve_path)) {
      continue;
    }
    add_mapping(ph, vmmap[i].kve_path, (uintptr_t)vmmap[i].kve_start,
                (uintptr_t)vmmap[i].kve_end, (uintptr_t)vmmap[i].kve_offset);
  }
  free(vmmap);

  list = (*env)->GetObjectField(env, this_obj, loadObjectList_ID);
  if (list == NULL) {
    return;
  }
  for (lib = ph->libs; lib != NULL; lib = lib->next) {
    jstring name = (*env)->NewStringUTF(env, lib->name);
    jobject obj;
    CHECK_EXCEPTION;
    obj = (*env)->CallObjectMethod(env, this_obj, createLoadObject_ID, name,
                                   (jlong)(lib->end - lib->base),
                                   (jlong)lib->base);
    CHECK_EXCEPTION;
    (*env)->CallBooleanMethod(env, list, listAdd_ID, obj);
    CHECK_EXCEPTION;
    (*env)->DeleteLocalRef(env, obj);
    (*env)->DeleteLocalRef(env, name);
  }
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
  loadObjectList_ID = (*env)->GetFieldID(env, cls, "loadObjectList", "Ljava/util/List;");
  CHECK_EXCEPTION;

  createClosestSymbol_ID = (*env)->GetMethodID(env, cls, "createClosestSymbol",
      "(Ljava/lang/String;J)Lsun/jvm/hotspot/debugger/cdbg/ClosestSymbol;");
  CHECK_EXCEPTION;
  createLoadObject_ID = (*env)->GetMethodID(env, cls, "createLoadObject",
      "(Ljava/lang/String;JJ)Lsun/jvm/hotspot/debugger/cdbg/LoadObject;");
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
  /* Bytes, as on every other platform: MachineDescription multiplies. */
  return (jint)sizeof(uintptr_t);
}

/*
 * Class:     sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal
 * Method:    attach0
 * Signature: (I)V
 */
JNIEXPORT void JNICALL Java_sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal_attach0__I
  (JNIEnv *env, jobject this_obj, jint jpid) {
  pid_t pid = (pid_t)jpid;
  struct ps_prochandle* ph;
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

  ph = (struct ps_prochandle*)calloc(1, sizeof(*ph));
  if (ph == NULL) {
    ptrace(PT_DETACH, pid, (void*)1, 0);
    THROW_NEW_DEBUGGER_EXCEPTION("out of memory");
  }
  ph->pid = pid;
  (*env)->SetLongField(env, this_obj, p_ps_prochandle_ID, (jlong)(intptr_t)ph);

  fill_load_objects(env, this_obj, ph);
}

/*
 * Class:     sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal
 * Method:    attach0
 * Signature: (Ljava/lang/String;Ljava/lang/String;)V
 */
JNIEXPORT void JNICALL
Java_sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal_attach0__Ljava_lang_String_2Ljava_lang_String_2
  (JNIEnv *env, jobject this_obj, jstring execName, jstring coreName) {
  THROW_NEW_DEBUGGER_EXCEPTION("core file debugging is not implemented here");
}

/*
 * Class:     sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal
 * Method:    detach0
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal_detach0
  (JNIEnv *env, jobject this_obj) {
  struct ps_prochandle* ph = get_proc(env, this_obj);
  if (ph != NULL) {
    ptrace(PT_DETACH, ph->pid, (void*)1, 0);
    free_libs(ph->libs);
    free(ph);
    (*env)->SetLongField(env, this_obj, p_ps_prochandle_ID, (jlong)0);
  }
}

/*
 * Class:     sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal
 * Method:    lookupByName0
 * Signature: (Ljava/lang/String;Ljava/lang/String;)J
 */
JNIEXPORT jlong JNICALL Java_sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal_lookupByName0
  (JNIEnv *env, jobject this_obj, jstring objectName, jstring symbolName) {
  struct ps_prochandle* ph = get_proc(env, this_obj);
  const char* objstr = NULL;
  const char* symstr;
  lib_info* lib;
  jlong result = 0;

  if (ph == NULL) {
    return 0;
  }
  if (objectName != NULL) {
    objstr = (*env)->GetStringUTFChars(env, objectName, NULL);
    CHECK_EXCEPTION_(0);
  }
  symstr = (*env)->GetStringUTFChars(env, symbolName, NULL);
  if (symstr == NULL) {
    if (objstr != NULL) {
      (*env)->ReleaseStringUTFChars(env, objectName, objstr);
    }
    return 0;
  }

  /*
   * The named object is tried first, then every other one.  Linux skips
   * the name altogether because the caller's idea of it -- "libjvm.so" --
   * need not be the path the kernel reports, and the agent also asks for
   * flags such as UseSharedSpaces with no object named at all.
   */
  lib = find_lib(ph, objstr);
  if (lib != NULL) {
    result = lookup_in_lib(lib, symstr);
  }
  if (result == 0) {
    for (lib = ph->libs; lib != NULL && result == 0; lib = lib->next) {
      result = lookup_in_lib(lib, symstr);
    }
  }

  (*env)->ReleaseStringUTFChars(env, symbolName, symstr);
  if (objstr != NULL) {
    (*env)->ReleaseStringUTFChars(env, objectName, objstr);
  }
  return result;
}

/*
 * Class:     sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal
 * Method:    lookupByAddress0
 * Signature: (J)Lsun/jvm/hotspot/debugger/cdbg/ClosestSymbol;
 */
JNIEXPORT jobject JNICALL Java_sun_jvm_hotspot_debugger_bsd_BsdDebuggerLocal_lookupByAddress0
  (JNIEnv *env, jobject this_obj, jlong address) {
  struct ps_prochandle* ph = get_proc(env, this_obj);
  uintptr_t target = (uintptr_t)address;
  lib_info* lib;
  jobject result = NULL;

  if (ph == NULL) {
    return NULL;
  }
  for (lib = ph->libs; lib != NULL; lib = lib->next) {
    char* addr;
    size_t size;
    const Elf64_Ehdr* eh;
    struct find_addr f;
    uintptr_t bias;

    if (target < lib->base || target >= lib->end) {
      continue;
    }
    if (map_file(lib->name, &addr, &size) != 0) {
      continue;
    }
    if ((eh = elf_header(addr, size)) != NULL) {
      bias = lib->base - elf_vaddr0(addr, size, eh);
      memset(&f, 0, sizeof(f));
      f.target = target - bias;
      elf_walk_symbols(addr, size, eh, visit_addr, &f);
      if (f.found) {
        jstring name = (*env)->NewStringUTF(env, f.name);
        if (name != NULL) {
          result = (*env)->CallObjectMethod(env, this_obj,
              createClosestSymbol_ID, name, (jlong)(f.target - f.best));
        }
      }
    }
    munmap(addr, size);
    break;
  }
  return result;
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
