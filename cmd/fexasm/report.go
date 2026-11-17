package main

import (
	"fmt"
	"os"
	"sort"
	"strings"
)

type tally struct{ pass, fail, skip int }

func (t tally) total() int { return t.pass + t.fail + t.skip }

// summarize prints the per-directory table, the top failing mnemonics and unsupported forms,
// and the diff against a baseline; with md != "" it also writes the markdown report body.
func summarize(rs []Result, base map[string]Result, md string) {
	var sb strings.Builder
	byDir := map[string]*tally{}
	var all tally
	reasons := map[string]int{}
	for _, r := range rs {
		t := byDir[r.Dir]
		if t == nil {
			t = &tally{}
			byDir[r.Dir] = t
		}
		switch r.Status {
		case "PASS":
			t.pass++
			all.pass++
		case "FAIL":
			t.fail++
			all.fail++
		default:
			t.skip++
			all.skip++
			reasons[reasonClass(r.Reason)]++
		}
	}
	dirs := make([]string, 0, len(byDir))
	for d := range byDir {
		dirs = append(dirs, d)
	}
	sort.Strings(dirs)
	fmt.Fprintf(&sb, "| Directory | Total | PASS | FAIL | SKIP |\n|---|---:|---:|---:|---:|\n")
	for _, d := range dirs {
		t := byDir[d]
		fmt.Fprintf(&sb, "| %s | %d | %d | %d | %d |\n", d, t.total(), t.pass, t.fail, t.skip)
	}
	fmt.Fprintf(&sb, "| **All** | **%d** | **%d** | **%d** | **%d** |\n\n", all.total(), all.pass, all.fail, all.skip)

	fmt.Fprintf(&sb, "### Skip reasons\n\n| Reason | Tests |\n|---|---:|\n")
	for _, k := range sortedByCount(reasons) {
		fmt.Fprintf(&sb, "| %s | %d |\n", k, reasons[k])
	}

	// Mnemonics of failing tests: tests containing the mnemonic that fail vs. that ran.
	type mstat struct{ fails, ran int }
	ms := map[string]*mstat{}
	failReasons := map[string]int{}
	forms := map[string]int{}
	for _, r := range rs {
		if r.Status == "FAIL" {
			failReasons[r.Reason]++
		}
		for _, f := range r.Forms {
			forms[f]++
		}
		if r.Status == "SKIP" && r.Reason != "unsupported-instruction" {
			continue
		}
		for _, m := range r.Mnemonics {
			s := ms[m]
			if s == nil {
				s = &mstat{}
				ms[m] = s
			}
			s.ran++
			if r.Status == "FAIL" {
				s.fails++
			}
		}
	}
	fmt.Fprintf(&sb, "\n### FAIL kinds\n\n| Kind | Tests |\n|---|---:|\n")
	for _, k := range sortedByCount(failReasons) {
		fmt.Fprintf(&sb, "| %s | %d |\n", k, failReasons[k])
	}
	names := make([]string, 0, len(ms))
	for m, s := range ms {
		if s.fails > 0 {
			names = append(names, m)
		}
	}
	sort.Slice(names, func(i, j int) bool {
		a, b := ms[names[i]], ms[names[j]]
		if a.fails != b.fails {
			return a.fails > b.fails
		}
		return names[i] < names[j]
	})
	fmt.Fprintf(&sb, "\n### Top failing mnemonics (FAIL tests containing the mnemonic; setup MOV/LEA/CALL/JMP/PUSH/POP excluded)\n\n| Mnemonic | Failing tests | Tests containing it |\n|---|---:|---:|\n")
	for i, m := range names {
		if i >= 40 {
			break
		}
		fmt.Fprintf(&sb, "| %s | %d | %d |\n", m, ms[m].fails, ms[m].ran)
	}
	byRate := append([]string(nil), names...)
	sort.Slice(byRate, func(i, j int) bool {
		a, b := ms[byRate[i]], ms[byRate[j]]
		ra, rb := a.fails*1000/a.ran, b.fails*1000/b.ran
		if ra != rb {
			return ra > rb
		}
		if a.fails != b.fails {
			return a.fails > b.fails
		}
		return byRate[i] < byRate[j]
	})
	fmt.Fprintf(&sb, "\n### Mnemonics with the highest failure rate (at least 3 failing tests)\n\nA test names many mnemonics, so the first table is dominated by setup instructions (ADD, CMP, ...). The rate separates the instruction under test from the setup.\n\n| Mnemonic | Failing | Containing | Rate |\n|---|---:|---:|---:|\n")
	shown := 0
	for _, m := range byRate {
		if ms[m].fails < 3 || shown >= 40 {
			continue
		}
		shown++
		fmt.Fprintf(&sb, "| %s | %d | %d | %d%% |\n", m, ms[m].fails, ms[m].ran, ms[m].fails*100/ms[m].ran)
	}
	fmt.Fprintf(&sb, "\n### Top unsupported forms at translate time (tests, skipped as unsupported-instruction)\n\n| Form | Tests |\n|---|---:|\n")
	for i, f := range sortedByCount(forms) {
		if i >= 40 {
			break
		}
		fmt.Fprintf(&sb, "| %s | %d |\n", f, forms[f])
	}

	if base != nil {
		var reg, fix, other []string
		for _, r := range rs {
			b, ok := base[r.Name]
			if !ok || b.Status == r.Status {
				continue
			}
			line := fmt.Sprintf("%s: %s -> %s", r.Name, b.Status, r.Status)
			switch {
			case b.Status == "PASS":
				reg = append(reg, line)
			case r.Status == "PASS":
				fix = append(fix, line)
			default:
				other = append(other, line)
			}
		}
		fmt.Fprintf(&sb, "\n### Diff against baseline\n\nregressions %d, newly passing %d, other changes %d\n\n", len(reg), len(fix), len(other))
		for _, l := range append(append(reg, fix...), other...) {
			fmt.Fprintf(&sb, "- %s\n", l)
		}
	}
	fmt.Print(sb.String())
	if md != "" {
		os.WriteFile(md, []byte(sb.String()), 0o644)
	}
}

func reasonClass(r string) string {
	if i := strings.Index(r, ":"); i > 0 && (strings.HasPrefix(r, "host-feature") || strings.HasPrefix(r, "syscall")) {
		return r[:i] + " (" + r[i+1:] + ")"
	}
	if strings.HasPrefix(r, "runtime-helper") {
		return r
	}
	return r
}

func sortedByCount(m map[string]int) []string {
	k := make([]string, 0, len(m))
	for x := range m {
		k = append(k, x)
	}
	sort.Slice(k, func(i, j int) bool {
		if m[k[i]] != m[k[j]] {
			return m[k[i]] > m[k[j]]
		}
		return k[i] < k[j]
	})
	return k
}
