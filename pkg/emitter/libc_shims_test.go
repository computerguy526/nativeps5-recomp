package emitter

import (
	"testing"

	"ps4-recomp/pkg/elfloader"
)

// Every HLE libc function binds by plain name (payload-SDK ELFs) and by NID (retail titles).
func TestLibcShimsBindByNameAndNID(t *testing.T) {
	for _, n := range libcShims {
		want := "shim_" + n
		if n == "abort" {
			want = "shim_guest_abort" // diagnostics hook from m1-setjmp (shims_eh.go) wins over libc
		}
		if got, ok := LookupShim(n); !ok || got != want {
			t.Errorf("LookupShim(%q) = %q,%v want %q", n, got, ok, want)
		}
		nid := elfloader.CalculateNID(n)
		if nid == "" {
			t.Errorf("no NID for %q", n)
			continue
		}
		if got, ok := LookupShim(nid + "#A#B"); !ok || got == "" {
			t.Errorf("LookupShim(NID %s of %q) not bound", nid, n)
		}
		if n != "abort" && !isFallbackShim(want) { // abort deliberately overrides (shims_eh.go)
			t.Errorf("%s must be a fallback shim", want)
		}
	}
}

// NIDs observed in the Syobon eboot import table (work/s2 syobon_eboot.elf) resolve to the libc shims.
func TestLibcShimsSyobonNIDs(t *testing.T) {
	cases := []struct{ name, nid string }{
		{"_Exit", "wZi5ly2guNw"},
		{"__cxa_atexit", "tsvEmnenz48"},
		{"_init_env", "bzQExy189ZI"},
		{"abort", "L1SBTkC+Cvw"},
		{"atexit", "8G2LB+A3rzg"},
		{"atof", "SRI6S9B+-a4"},
		{"atoi", "fPxypibz2MY"},
		{"bsearch", "NesIgTmfF0Q"},
		{"calloc", "2X5agFjKxMc"},
		{"dlclose", "oiT0vgzmW0M"},
		{"dlerror", "ucFJiTO1EUw"},
		{"dlopen", "UteVS6B1ZrU"},
		{"dlsym", "biY+kcVB5D4"},
		{"execvp", "0NJDcQ2B8vE"},
		{"fclose", "uodLYyUip20"},
		{"fdopen", "qdlHjTa9hQ4"},
		{"ferror", "AHxyhN96dy4"},
		{"fflush", "MUjC4lbHrK4"},
		{"fgetc", "AEuF3F2f8TA"},
		{"fgets", "KdP-nULpuGw"},
		{"fileno", "Fm-dmyywH9Q"},
		{"fopen", "xeYO4u7uyJ0"},
		{"fork", "cjt-4ySmYTs"},
		{"fprintf", "fffwELXNVFA"},
		{"fputc", "aZK8lNei-Qw"},
		{"fread", "lbB+UlZqVG0"},
		{"free", "tIhsqj0qsFE"},
		{"fseek", "rQFVBXp-Cxg"},
		{"fseeko", "pkYiKw09PRA"},
		{"ftell", "Qazy8LmXTvw"},
		{"ftello", "5qP1iVQkdck"},
		{"fwrite", "MpxhMh8QFro"},
		{"getenv", "smbQukfxYJM"},
		{"gmtime", "1mecP7RgI2A"},
		{"isalnum", "4uJJNi+C9wk"},
		{"isalpha", "+xU0WKT8mDc"},
		{"isblank", "lhnrCOBiTGo"},
		{"iscntrl", "akpGErA1zdg"},
		{"isgraph", "rrgxakQtvc0"},
		{"islower", "KqYTqtSfGos"},
		{"isprint", "eGkOpTojJl4"},
		{"ispunct", "I6Z-684E2C4"},
		{"isspace", "wazw2x2m3DQ"},
		{"isupper", "GcFKlTJEMkI"},
		{"isxdigit", "srzSVSbKn7M"},
		{"kill", "W0xkN0+ZkCE"},
		{"malloc", "gQX+4GDQjpM"},
		{"malloc_usable_size", "NDcSfcYZRC8"},
		{"mbrtowc", "qVHpv0PxouI"},
		{"memchr", "8u8lPzUEq+U"},
		{"memcmp", "DfivPArhucg"},
		{"perror", "EMutwaQ34Jo"},
		{"posix_memalign", "cVSk9y8URbc"},
		{"pthread_set_name_np", "oxMp8uPqa+U"},
		{"qsort", "AEJdIVZTEmo"},
		{"rand", "cpCOXWMgha0"},
		{"realloc", "Y7aJ1uydPMo"},
		{"remove", "MZO7FXyAPU8"},
		{"setenv", "M4YYbSFfJ8g"},
		{"sigaddset", "JUimFtKe0Kc"},
		{"sigemptyset", "+F7C-hdk7+E"},
		{"sleep", "0wu33hunNdE"},
		{"snprintf", "eLdDw6l0-bU"},
		{"sprintf", "tcVi5SivF7Q"},
		{"srand", "VPbJwTCgME0"},
		{"strcasecmp", "AV6ipCNa4Rw"},
		{"strcat", "Ls4tzzhimqQ"},
		{"strchr", "ob5xAW4ln-0"},
		{"strdup", "g7zzzLDYGw0"},
		{"strerror", "RIa6GnWp+iU"},
		{"strlcat", "ByfjUZsWiyg"},
		{"strlcpy", "SfQIZcqvvms"},
		{"strncasecmp", "pXvbDfchu6k"},
		{"strncmp", "aesyjrHVWy4"},
		{"strrchr", "9yDWMxEFdJU"},
		{"strstr", "viiwFMaNamA"},
		{"strtod", "2vDqwBlpF-o"},
		{"strtok_r", "enqPGLfmVNU"},
		{"strtol", "mXlxhmLNMPg"},
		{"strtoll", "VOBg+iNwB-4"},
		{"strtoul", "QxmSHBCuKTk"},
		{"strtoull", "5OqszGpy7Mg"},
		{"sysctl", "DFmMT80xcNI"},
		{"sysctlbyname", "MhC53TKmjVA"},
		{"time", "wLlFkwG9UcQ"},
		{"tolower", "PqF+kHW-2WQ"},
		{"toupper", "TYE4irxSmko"},
		{"ungetc", "-LFO7jhD5CE"},
		{"vfprintf", "pDBDcY6uLSA"},
		{"vsnprintf", "Q2V+iqvjgC0"},
		{"vsprintf", "jbz9I9vkqkk"},
		{"vsscanf", "UTrpOVLcoOA"},
		{"waitpid", "dTnYqqLIQ6c"},
		{"wcrtomb", "y9OoA+P5cjk"},
		{"wcscmp", "pNtJdE3x49E"},
		{"wcslen", "WkkeywLJcgU"},
		{"wcsncmp", "E8wCoUEbfzk"},
		{"wcsstr", "WDpobjImAb4"},
		{"wcstombs", "v7S7LhP2OJc"},
	}
	for _, c := range cases {
		got, ok := LookupShim(c.nid + "#H#I")
		want := "shim_" + c.name
		if c.name == "abort" {
			want = "shim_guest_abort"
		}
		if !ok || got != want {
			t.Errorf("NID %s (%s) -> %q,%v want shim_%s", c.nid, c.name, got, ok, c.name)
		}
	}
}

// Existing shims keep binding unconditionally; libc fallbacks do not replace guest definitions.
func TestFallbackShimClassification(t *testing.T) {
	if isFallbackShim("shim_memcpy") || isFallbackShim("shim_sceKernelWrite") {
		t.Fatal("pre-existing shims must not be fallback shims")
	}
	if !isFallbackShim("shim_strstr") || !isFallbackShim("shim_malloc") {
		t.Fatal("libc shims must be fallback shims")
	}
}
