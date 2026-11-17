// SPDX-License-Identifier: GPL-2.0-or-later

package thunkgen

// The libkernel subset M1 uses: output, the mmap family, mutexes, threads, TLS, exit.
// Implementations: pkg/runtime/kernel/{syscalls,memory,sync,threading}. The thunk symbols are
// the names emitter/shims.go already binds imports to.

var ctxArg = Arg{Kind: Ctx, CType: "GuestContext *", Name: "ctx"}

func sc(ctype, name string) Arg  { return Arg{Kind: Scalar, CType: ctype, Name: name} }
func ptr(ctype, name string) Arg { return Arg{Kind: Ptr, CType: ctype, Name: name} }
func gaddr(name string) Arg      { return Arg{Kind: GuestAddr, CType: "uint64_t", Name: name} }
func slot(name string) Arg       { return Arg{Kind: PtrSlot, CType: "void **", Name: name} }

// iovec: { void *iov_base; size_t iov_len; }, the same 16 bytes on FreeBSD/amd64 and Darwin/arm64.
var iovec = &Struct{CType: "struct iovec", Size: 16, PtrFields: []int{0}}

func iovArg(countIdx int) Arg {
	return Arg{Kind: StructArray, CType: "const struct iovec *", Name: "iov", Elem: iovec, Count: countIdx, Max: 1024}
}

func mutexOp(shim, hle string) Desc {
	return Desc{Group: "sync", Shim: shim, HLE: hle, CRet: "int", Ret: RetInt, Args: []Arg{ctxArg, ptr("void *", "mutex")}}
}

// leanMutexOp is a mutex operation on the lean bridge: the HLE function takes a host lock (it may wait,
// which is fine with the guest registers stored in the context) and never enters guest code.
func leanMutexOp(shim, hle string) Desc {
	d := mutexOp(shim, hle)
	d.Lean = true
	return d
}

// Libkernel is the descriptor table.
var Libkernel = []Desc{
	// ---- output ----
	{Group: "sys", Shim: "shim_write", HLE: "hle_write", CRet: "ssize_t", Ret: RetInt,
		Args: []Arg{ctxArg, sc("int", "fd"), ptr("const void *", "buf"), sc("size_t", "count")}},
	{Group: "sys", Shim: "shim_writev", HLE: "hle_writev", CRet: "ssize_t", Ret: RetInt,
		Args: []Arg{ctxArg, sc("int", "fd"), iovArg(3), sc("int", "iovcnt")}},
	{Group: "sys", Shim: "shim_sceKernelWrite", HLE: "hle_sceKernelWrite", CRet: "int64_t", Ret: RetInt,
		Notes: "sceKernel* results: >= 0, or an SCE_KERNEL_ERROR_* code zero-extended in RAX as the kernel library leaves it.",
		Args:  []Arg{ctxArg, sc("int", "fd"), ptr("const void *", "buf"), sc("size_t", "count")}},
	{Group: "sys", Shim: "shim_sceKernelWritev", HLE: "hle_sceKernelWritev", CRet: "int64_t", Ret: RetInt,
		Args: []Arg{ctxArg, sc("int", "fd"), iovArg(3), sc("int", "iovcnt")}},
	{Group: "sys", Shim: "shim_exit", HLE: "hle_exit", CRet: "void", Ret: RetNoReturn, Args: []Arg{ctxArg, sc("int", "status")}},

	// ---- mappings: addresses are host pointers into the arena; NULL means "no address" ----
	{Group: "sys", Shim: "shim_mmap", HLE: "hle_mmap", CRet: "void *", Ret: RetPtrOrFail,
		Args: []Arg{ctxArg, ptr("void *", "addr"), sc("size_t", "len"), sc("int", "prot"), sc("int", "flags"), sc("int", "fd"), sc("off_t", "offset")}},
	{Group: "sys", Shim: "shim_munmap", HLE: "hle_munmap", CRet: "int", Ret: RetInt,
		Args: []Arg{ctxArg, ptr("void *", "addr"), sc("size_t", "len")}},
	{Group: "sys", Shim: "shim_sceKernelMmap", HLE: "hle_sceKernelMmap", CRet: "int", Ret: RetInt,
		Notes: "The seventh argument (res) arrives on the guest stack.",
		Args:  []Arg{ctxArg, ptr("void *", "addr"), sc("size_t", "len"), sc("int", "prot"), sc("int", "flags"), sc("int", "fd"), sc("off_t", "offset"), slot("res")}},
	{Group: "mem", Shim: "shim_sceKernelMapFlexibleMemory", HLE: "hle_sceKernelMapFlexibleMemory", CRet: "int", Ret: RetInt,
		Args: []Arg{ctxArg, slot("addr_in_out"), sc("size_t", "len"), sc("int", "prot"), sc("int", "flags")}},
	{Group: "mem", Shim: "shim_sceKernelMapNamedFlexibleMemory", HLE: "hle_sceKernelMapNamedFlexibleMemory", CRet: "int", Ret: RetInt,
		Args: []Arg{ctxArg, slot("addr_in_out"), sc("size_t", "len"), sc("int", "prot"), sc("int", "flags"), ptr("const char *", "name")}},
	{Group: "mem", Shim: "shim_sceKernelReserveVirtualRange", HLE: "hle_sceKernelReserveVirtualRange", CRet: "int", Ret: RetInt,
		Args: []Arg{ctxArg, slot("addr_in_out"), sc("size_t", "len"), sc("int", "flags"), sc("size_t", "alignment")}},
	{Group: "mem", Shim: "shim_sceKernelMapDirectMemory", HLE: "hle_sceKernelMapDirectMemory", CRet: "int", Ret: RetInt,
		Args: []Arg{ctxArg, slot("addr_in_out"), sc("size_t", "len"), sc("int", "prot"), sc("int", "flags"), sc("off_t", "phys"), sc("size_t", "alignment")}},
	{Group: "mem", Shim: "shim_sceKernelMprotect", HLE: "hle_sceKernelMprotect", CRet: "int", Ret: RetInt,
		Args: []Arg{ctxArg, ptr("const void *", "addr"), sc("size_t", "len"), sc("int", "prot")}},

	// ---- mutexes: the guest's pthread_mutex_t is one word the HLE owns; never dereferenced by the guest ----
	{Group: "sync", Shim: "shim_pthread_mutex_init", HLE: "hle_pthread_mutex_init", CRet: "int", Ret: RetInt,
		Args: []Arg{ctxArg, ptr("void *", "mutex"), ptr("const void *", "attr")}},
	leanMutexOp("shim_pthread_mutex_lock", "hle_pthread_mutex_lock"),
	leanMutexOp("shim_pthread_mutex_trylock", "hle_pthread_mutex_trylock"),
	leanMutexOp("shim_pthread_mutex_unlock", "hle_pthread_mutex_unlock"),
	mutexOp("shim_pthread_mutex_destroy", "hle_pthread_mutex_destroy"),
	{Group: "sync", Shim: "shim_scePthreadMutexInit", HLE: "hle_scePthreadMutexInit", CRet: "int", Ret: RetInt,
		Args: []Arg{ctxArg, ptr("void *", "mutex"), ptr("const void *", "attr")}},
	leanMutexOp("shim_scePthreadMutexLock", "hle_scePthreadMutexLock"),
	leanMutexOp("shim_scePthreadMutexTrylock", "hle_scePthreadMutexTrylock"),
	leanMutexOp("shim_scePthreadMutexUnlock", "hle_scePthreadMutexUnlock"),
	mutexOp("shim_scePthreadMutexDestroy", "hle_scePthreadMutexDestroy"),

	// ---- threads: entry and arg are guest values handed back to guest code ----
	{Group: "thread", Shim: "shim_pthread_create", HLE: "hle_pthread_create", CRet: "int", Ret: RetInt,
		Args: []Arg{ctxArg, ptr("uint64_t *", "thread"), ptr("const uint64_t *", "attr"), gaddr("entry"), gaddr("arg")}},
	{Group: "thread", Shim: "shim_scePthreadCreate", HLE: "hle_scePthreadCreate", CRet: "int", Ret: RetInt,
		Args: []Arg{ctxArg, ptr("uint64_t *", "thread"), ptr("const uint64_t *", "attr"), gaddr("entry"), gaddr("arg"), ptr("const char *", "name")}},
	{Group: "thread", Shim: "shim_pthread_join", HLE: "hle_pthread_join", CRet: "int", Ret: RetInt,
		Args: []Arg{ctxArg, gaddr("thread"), ptr("uint64_t *", "retval")}},
	{Group: "thread", Shim: "shim_scePthreadJoin", HLE: "hle_scePthreadJoin", CRet: "int", Ret: RetInt,
		Args: []Arg{ctxArg, gaddr("thread"), ptr("uint64_t *", "retval")}},

	// ---- TLS: returns a pointer into the calling thread's TLS block ----
	{Group: "thread", Shim: "shim___tls_get_addr", HLE: "hle___tls_get_addr", CRet: "void *", Ret: RetPtr,
		Args: []Arg{ctxArg, ptr("const uint64_t *", "ti")}},
}

// LibkernelHeader is the generated prototype header, relative to pkg/runtime.
const LibkernelHeader = "modules/libkernel/nps5_libkernel_hle.h"

// LibkernelThunkFile returns the generated thunk source of a group, relative to pkg/runtime:
// sys (kernel/syscalls), mem (kernel/memory), sync (kernel/sync), thread (kernel/threading).
func LibkernelThunkFile(group string) string {
	return "modules/libkernel/nps5_thunks_" + group + ".c"
}

// Groups returns the descriptors by group.
func Groups(descs []Desc) map[string][]Desc {
	out := make(map[string][]Desc)
	for _, d := range descs {
		out[d.Group] = append(out[d.Group], d)
	}
	return out
}
