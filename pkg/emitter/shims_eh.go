package emitter

// Shims for the bring-up of non-local control flow (docs/m1/setjmp-eh.md). Kept out of shims.go so
// the m1-libc work, which edits that file, merges without conflicts.
func init() {
	// abort() names the guest function that threw an uncaught exception (nps5_eh.c). Deliberately
	// overrides any other binding of "abort".
	CanonicalShims["abort"] = "shim_guest_abort"
}
