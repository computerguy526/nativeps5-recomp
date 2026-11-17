#!/usr/bin/env python3
"""Builds docs/m1/memtests.md from `go test -json` output and the guest RESULT file.
usage: memtests_report.py <go.json> <guest.txt|-> <out.md> [--findings <findings.md>]
Prints the PASS/FAIL/KNOWN-FAIL/SKIP counts on stdout."""
import json, re, sys, collections

def area_of(pkg, test):
    if pkg.endswith("thunkgen"): return "thunk"
    if pkg.endswith("elfloader"): return "loader"
    if pkg.endswith("guestmem"): return "layout/mode"
    t = test
    if t.startswith("TestMemArena"): return "arena"
    if t.startswith("TestMemVM"): return "mapping/translation"
    if t.startswith("TestMemConc"): return "concurrency"
    if t.startswith("TestMemIdentityThunk"): return "translation"
    return "misc"

def main():
    args = sys.argv[1:]
    findings = None
    if "--findings" in args:
        i = args.index("--findings"); findings = args[i + 1]; del args[i:i + 2]
    gojson, guest, out = args[0], args[1], args[2]
    rows = []  # (name, area, result, detail)
    tests = {}
    outputs = collections.defaultdict(list)
    for line in open(gojson, errors="replace"):
        try: ev = json.loads(line)
        except Exception: continue
        t = ev.get("Test")
        if not t: continue
        key = (ev["Package"], t)
        if ev["Action"] == "output": outputs[key].append(ev["Output"])
        elif ev["Action"] in ("pass", "fail", "skip"): tests[key] = ev["Action"]
    mine = re.compile(r'^(TestMem|TestThunkExecution|TestRejectsUnclassifiable|TestStackArgumentIndexing|TestDescriptorValidation)')
    tests = {k: v for k, v in tests.items() if mine.match(k[1])}
    names = sorted(tests)
    parents = set()
    for (p, t) in names:
        for (p2, t2) in names:
            if p == p2 and t2.startswith(t + "/"): parents.add((p, t))
    for key in names:
        if key in parents: continue
        pkg, t = key
        act = tests[key]
        text = "".join(outputs[key])
        short = pkg.split("/pkg/")[-1] if "/pkg/" in pkg else pkg
        short = re.sub(r"^ps4-recomp/pkg/", "", pkg)
        m = re.search(r"KNOWN-FAIL(?: \(strict\))?: (.*)", text)
        if act == "pass": res, det = "PASS", ""
        elif act == "skip":
            if m: res, det = "KNOWN-FAIL", m.group(1).strip()
            else:
                s = re.search(r"\n\s*\S+\.go:\d+: (.*)", text)
                res, det = "SKIP", (s.group(1).strip() if s else "")
        else:
            lines = [re.sub(r"^\s*\S+\.go:\d+: ", "", l.strip()) for l in text.splitlines() if re.search(r"\.go:\d+:", l)]
            res, det = "FAIL", "; ".join(lines)[:600]
        name = t
        # runtime subtests carry the C case name as the last path element
        last = t.split("/")[-1]
        if re.match(r"^(arena|vm|xlate|conc)\.", last): name = last
        else: name = f"{short}: {t}"
        rows.append((name, area_of(pkg, t), res, det))
    if guest != "-":
        try:
            for line in open(guest, errors="replace"):
                p = line.rstrip("\n").split("|", 4)
                if len(p) == 5 and p[0] == "RESULT": rows.append((p[1], p[2], p[3], p[4]))
        except FileNotFoundError: pass
    # de-duplicate by name (a case can appear twice when a guest program also reports a crash row)
    c = collections.Counter(r[2] for r in rows)
    def esc(s): return s.replace("|", "\\|").replace("\n", " ")
    order = {"FAIL": 0, "KNOWN-FAIL": 1, "SKIP": 2, "PASS": 3}
    with open(out, "w") as f:
        f.write(open(findings).read() if findings else "")
        f.write("\n## Results\n\n")
        f.write(f"PASS {c['PASS']} | FAIL {c['FAIL']} | KNOWN-FAIL {c['KNOWN-FAIL']} | SKIP {c['SKIP']} | total {len(rows)}\n\n")
        f.write("| test | area | result | failure detail |\n|---|---|---|---|\n")
        for name, area, res, det in sorted(rows, key=lambda r: (order.get(r[2], 9), r[1], r[0])):
            f.write(f"| {esc(name)} | {esc(area)} | {res} | {esc(det)} |\n")
    print(f"PASS {c['PASS']} FAIL {c['FAIL']} KNOWN-FAIL {c['KNOWN-FAIL']} SKIP {c['SKIP']} total {len(rows)}")
    sys.exit(1 if c["FAIL"] else 0)

main()
