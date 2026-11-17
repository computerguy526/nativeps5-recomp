package diag

import (
	"bytes"
	"fmt"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

// PCMap entry flags (bits 56..63 of the guest word in the __nps5_pcmap section).
const (
	FlagUnsupported         = 1 // instruction lowered to a brk stub
	FlagUnsupportedIndirect = 2 // unsupported indirect jmp/call
	FlagFuncEntry           = 4 // first entry of a function
	FlagEnd                 = 8 // pcmap.bin sentinel: the end of a function's host code, not a guest instruction
)

// Addr is a guest address that marshals to JSON as a "0x..." string.
type Addr uint64

func (a Addr) MarshalJSON() ([]byte, error) { return []byte(fmt.Sprintf(`"0x%x"`, uint64(a))), nil }

func (a *Addr) UnmarshalJSON(b []byte) error {
	v, err := strconv.ParseUint(strings.TrimPrefix(strings.Trim(string(b), `"`), "0x"), 16, 64)
	*a = Addr(v)
	return err
}

// FuncInfo is what the census needs to know about one discovered guest function.
type FuncInfo struct {
	Entry, End   uint64
	Name         string
	JumpTables   map[uint64]int // indirect jmp PC -> number of recovered targets
	IndirectJmps []uint64       // jmp reg / jmp [mem] sites that are not RIP-relative GOT slots
	IndirectCall int            // call reg / call [mem]
	GotTailJmps  int            // jmp [rip+x] (import tail calls)
}

// UnsupportedHit is one emitted unsupported-instruction stub.
type UnsupportedHit struct {
	Addr     Addr   `json:"guest_addr"`
	Func     Addr   `json:"function,omitempty"`
	Mnemonic string `json:"mnemonic"`
	Form     string `json:"operand_form"`
	Text     string `json:"text,omitempty"`
	Kind     string `json:"kind"` // "instruction" or "indirect_jmp"/"indirect_call"
}

type FormCount struct {
	Mnemonic string `json:"mnemonic"`
	Form     string `json:"operand_form"`
	Count    int    `json:"count"`
}

// Census is census.json.
type Census struct {
	FunctionsFound     int              `json:"functions_found"`
	FunctionsEmitted   int              `json:"functions_emitted"`
	ShimStubs          int              `json:"shim_stub_functions"`
	FDECount           int              `json:"fde_count"`
	GuestInstsEmitted  int              `json:"guest_insts_emitted"`
	UnsupportedTotal   int              `json:"unsupported_total"`
	UnsupportedPct     float64          `json:"unsupported_pct_of_insts"`
	UnsupportedByForm  []FormCount      `json:"unsupported_by_form"`
	Unsupported        []UnsupportedHit `json:"unsupported"`
	IndirectJmpSites   int              `json:"indirect_jmp_sites"`
	JumpTablesRecover  int              `json:"jump_tables_recovered"`
	JumpTableSitesOK   []Addr           `json:"jump_table_sites_recovered"`
	JumpTableSitesMiss []Addr           `json:"jump_table_sites_unrecovered"`
	GotTailJmps        int              `json:"got_tail_jmps"`
	IndirectCalls      int              `json:"indirect_calls"`
	UD2Stubs           int              `json:"ud2_stubs"`
}

// AddrMapFunc is one function of the addrmap.
type AddrMapFunc struct {
	Guest  uint64
	Symbol string
	Name   string
	Insts  int
	Chunk  string
}

// ScanResult is the outcome of scanning (and instrumenting) the emitted assembly.
type ScanResult struct {
	Census    Census
	Funcs     []AddrMapFunc
	AddrMap   []byte // addrmap.txt contents
	PCEntries int
}

var reHexTok = regexp.MustCompile(`^-?(0x[0-9a-fA-F]+|[0-9]+)$`)

// OperandForm renders the operand classes of an instruction text, e.g. "ymm,ymm,imm".
func OperandForm(text string) (mnem, form string) {
	text = strings.TrimSpace(text)
	if i := strings.Index(text, " [UNSUPPORTED"); i >= 0 {
		text = text[:i]
	}
	fields := strings.Fields(text)
	// drop instruction prefixes
	for len(fields) > 1 {
		switch strings.ToUpper(fields[0]) {
		case "LOCK", "REP", "REPE", "REPNE", "XACQUIRE", "XRELEASE", "DATA16", "ADDR32":
			fields = fields[1:]
			continue
		}
		break
	}
	if len(fields) == 0 {
		return "", ""
	}
	mnem = strings.ToUpper(fields[0])
	rest := strings.Join(fields[1:], " ")
	if rest == "" {
		return mnem, ""
	}
	var cls []string
	for _, op := range splitOperands(rest) {
		cls = append(cls, classifyOperand(op))
	}
	return mnem, strings.Join(cls, ",")
}

func splitOperands(s string) []string {
	var out []string
	depth, start := 0, 0
	for i, c := range s {
		switch c {
		case '[':
			depth++
		case ']':
			depth--
		case ',':
			if depth == 0 {
				out = append(out, strings.TrimSpace(s[start:i]))
				start = i + 1
			}
		}
	}
	return append(out, strings.TrimSpace(s[start:]))
}

func classifyOperand(op string) string {
	u := strings.ToUpper(strings.TrimSpace(op))
	switch {
	case strings.Contains(u, "["):
		return "m"
	case reHexTok.MatchString(op):
		return "imm"
	case strings.HasPrefix(u, "YMM") || (len(u) >= 2 && u[0] == 'Y' && u[1] >= '0' && u[1] <= '9'):
		return "ymm"
	case strings.HasPrefix(u, "XMM") || (len(u) >= 2 && u[0] == 'X' && u[1] >= '0' && u[1] <= '9'):
		return "xmm"
	case strings.HasPrefix(u, "ZMM"):
		return "zmm"
	case strings.HasPrefix(u, "ST") || strings.HasPrefix(u, "F"):
		if strings.HasPrefix(u, "ST") {
			return "st"
		}
	}
	if strings.HasPrefix(u, "REL") || strings.HasPrefix(u, ".") || strings.HasPrefix(u, "0X") {
		return "rel"
	}
	return "r"
}

// BlockLabel is one basic-block label of the emitted assembly (an addrmap "L" line).
type BlockLabel struct {
	PC    uint64
	Label string
	Func  uint64
}

// AsmFacts is the structured side channel of the emitter: everything the census and the
// addrmap need to know about the emitted assembly, collected while the assembly is written,
// so nothing has to parse the (comment-free) assembly text.
type AsmFacts struct {
	Funcs       []AddrMapFunc // every emitted guest symbol, shim stubs (Insts == 0) included
	Blocks      []BlockLabel
	Unsupported []UnsupportedHit // Mnemonic and Form are derived from Text by BuildScan
	GuestInsts  int
	UD2Stubs    int
	PCEntries   int // guest instructions with a pc-map entry
}

// BuildScan builds the census and the addrmap from the emitter's facts and the discovery data.
func BuildScan(facts *AsmFacts, funcs []FuncInfo, fdeCount int) *ScanResult {
	res := &ScanResult{Funcs: facts.Funcs, PCEntries: facts.PCEntries}
	c := &res.Census
	c.FDECount = fdeCount
	c.FunctionsFound = len(funcs)
	c.GuestInstsEmitted = facts.GuestInsts
	c.UD2Stubs = facts.UD2Stubs
	names := map[uint64]string{}
	for _, f := range funcs {
		if f.Name != "" {
			names[f.Entry] = f.Name
		}
	}
	forms := map[[2]string]int{}
	var am bytes.Buffer
	am.WriteString("# nps5 addrmap v1 (guest PC <-> host symbol/label <-> function)\n")
	am.WriteString("# F <guest entry> <host symbol> <name> <guest insts> <chunk file>\n")
	am.WriteString("# L <guest pc> <host label> <function entry>   (basic-block labels)\n")
	am.WriteString("# U <guest pc> <kind> <mnemonic> <operand form> (unsupported stubs)\n")
	for _, b := range facts.Blocks {
		fmt.Fprintf(&am, "L 0x%x %s 0x%x\n", b.PC, b.Label, b.Func)
	}
	for _, u := range facts.Unsupported {
		mn, form := OperandForm(u.Text)
		u.Mnemonic, u.Form = mn, form
		u.Text = strings.TrimSpace(u.Text)
		c.Unsupported = append(c.Unsupported, u)
		forms[[2]string{mn, form}]++
		fmt.Fprintf(&am, "U 0x%x %s %s %s\n", uint64(u.Addr), u.Kind, mn, form)
	}
	for i := range res.Funcs {
		f := &res.Funcs[i]
		if f.Name == "" {
			f.Name = names[f.Guest]
		}
		if f.Insts == 0 {
			c.ShimStubs++
		} else {
			c.FunctionsEmitted++
		}
		nm := f.Name
		if nm == "" {
			nm = "-"
		}
		fmt.Fprintf(&am, "F 0x%x %s %s %d %s\n", f.Guest, f.Symbol, strings.ReplaceAll(nm, " ", "_"), f.Insts, f.Chunk)
	}
	res.AddrMap = sortAddrMap(am.Bytes())

	for k, n := range forms {
		c.UnsupportedByForm = append(c.UnsupportedByForm, FormCount{k[0], k[1], n})
	}
	sort.Slice(c.UnsupportedByForm, func(i, j int) bool {
		a, b := c.UnsupportedByForm[i], c.UnsupportedByForm[j]
		if a.Count != b.Count {
			return a.Count > b.Count
		}
		return a.Mnemonic+a.Form < b.Mnemonic+b.Form
	})
	c.UnsupportedTotal = len(c.Unsupported)
	if c.GuestInstsEmitted > 0 {
		c.UnsupportedPct = 100 * float64(c.UnsupportedTotal) / float64(c.GuestInstsEmitted)
	}
	for _, f := range funcs {
		c.IndirectCalls += f.IndirectCall
		c.GotTailJmps += f.GotTailJmps
		for _, site := range f.IndirectJmps {
			c.IndirectJmpSites++
			if n := f.JumpTables[site]; n > 0 {
				c.JumpTableSitesOK = append(c.JumpTableSitesOK, Addr(site))
			} else {
				c.JumpTableSitesMiss = append(c.JumpTableSitesMiss, Addr(site))
			}
		}
		// Recovered tables not listed as indirect sites (e.g. via other idioms) still count.
		for site, n := range f.JumpTables {
			if n > 0 && !containsU64(f.IndirectJmps, site) {
				c.IndirectJmpSites++
				c.JumpTableSitesOK = append(c.JumpTableSitesOK, Addr(site))
			}
		}
	}
	c.JumpTablesRecover = len(c.JumpTableSitesOK)
	sort.Slice(c.JumpTableSitesOK, func(i, j int) bool { return c.JumpTableSitesOK[i] < c.JumpTableSitesOK[j] })
	sort.Slice(c.JumpTableSitesMiss, func(i, j int) bool { return c.JumpTableSitesMiss[i] < c.JumpTableSitesMiss[j] })
	if c.UnsupportedByForm == nil {
		c.UnsupportedByForm = []FormCount{}
	}
	if c.Unsupported == nil {
		c.Unsupported = []UnsupportedHit{}
	}
	return res
}

func containsU64(s []uint64, v uint64) bool {
	for _, x := range s {
		if x == v {
			return true
		}
	}
	return false
}

// sortAddrMap keeps the header comments first, then F, L, U lines in that order, each by guest
// address.
func sortAddrMap(in []byte) []byte {
	var hdr, f, l, u []string
	for _, ln := range strings.Split(strings.TrimRight(string(in), "\n"), "\n") {
		switch {
		case strings.HasPrefix(ln, "#"):
			hdr = append(hdr, ln)
		case strings.HasPrefix(ln, "F "):
			f = append(f, ln)
		case strings.HasPrefix(ln, "L "):
			l = append(l, ln)
		case strings.HasPrefix(ln, "U "):
			u = append(u, ln)
		}
	}
	key := func(ln string) uint64 {
		p := strings.Fields(ln)
		if len(p) < 2 {
			return 0
		}
		v, _ := strconv.ParseUint(strings.TrimPrefix(p[1], "0x"), 16, 64)
		return v
	}
	for _, s := range [][]string{f, l, u} {
		s := s
		sort.SliceStable(s, func(i, j int) bool { return key(s[i]) < key(s[j]) })
	}
	all := append(append(append(hdr, f...), l...), u...)
	return []byte(strings.Join(all, "\n") + "\n")
}
