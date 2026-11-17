#!/bin/bash
# Runs the whole guest-memory-model adversarial suite and writes docs/m1/memtests.md.
#   Go (race detector on):  pkg/guestmem, pkg/elfloader, pkg/thunkgen   TestMem*, thunk/loader/layout cases
#   Go + C host harnesses:  pkg/runtime TestMem* (arena_adv, vm_adv, xlate_adv, conc_adv, thunk asm)
#   PS5 guest programs:     tools/memtests_guest.sh (payload SDK, base and identity, run sandboxed)
# usage: tools/memtests.sh [--no-guest] [--strict] [--short] [--no-doc]
#   --strict  KNOWN-FAIL cases are reported as failures (what the fix loop runs until it is empty)
#   --short   1 s concurrency phases
# Never runs scripts/build.sh and never executes an x86 binary.
set -u
export NPS5_AUDIO_NULL="${NPS5_AUDIO_NULL:-1}"   # tests are silent (headless clock-paced audio device)
W="$(cd "$(dirname "$0")/.." && pwd)"; cd "$W"
GUEST=1; STRICT=0; SHORT=""; DOC=1
for a in "$@"; do case "$a" in --no-guest) GUEST=0;; --strict) STRICT=1;; --short) SHORT="-short";; --no-doc) DOC=0;; esac; done
export GOFLAGS=-mod=vendor
[ "$STRICT" = 1 ] && export MEMTESTS_STRICT=1
mkdir -p build/memtests runs home
GOJ=build/memtests/go.json; GUESTTXT=build/memtests/guest.txt
rm -f "$GOJ" "$GUESTTXT"
echo "== Go: guestmem, elfloader, thunkgen (-race)"
go test -race -count=1 -json ./pkg/guestmem ./pkg/elfloader ./pkg/thunkgen >> "$GOJ" 2>build/memtests/go.race.err
rc1=$?
echo "== Go + C host harnesses: pkg/runtime"
go test -count=1 -timeout 20m $SHORT -run 'TestMem' -json ./pkg/runtime >> "$GOJ" 2>build/memtests/go.rt.err
rc2=$?
rc3=0
if [ "$GUEST" = 1 ]; then
  echo "== PS5 guest programs (base + identity)"
  tools/memtests_guest.sh "$W/$GUESTTXT"; rc3=$?
fi
DOCARG="build/memtests/memtests.md"; [ "$DOC" = 1 ] && DOCARG="docs/m1/memtests.md"
FIND=""; [ -f docs/m1/memtests_findings.md ] && FIND="--findings docs/m1/memtests_findings.md"
python3 -I tools/memtests_report.py "$GOJ" "$([ "$GUEST" = 1 ] && echo "$GUESTTXT" || echo -)" "$DOCARG" $FIND
rc4=$?
echo "wrote $DOCARG (go: race=$rc1 runtime=$rc2 guest=$rc3 report=$rc4)"
exit $rc4
