#!/bin/bash
export NPS5_AUDIO_NULL="${NPS5_AUDIO_NULL:-1}"
# Builds and runs the SPEC 8.2 acceptance set: hello_world.elf, ps5_hello_k, and four broken programs
# (bad memory access, unsupported instruction, unresolved import, guest ud2). Bundles land in runs/.
# usage: tools/diag_acceptance.sh   (needs out/ps4-recomp: GOFLAGS=-mod=vendor go build -o out/ps4-recomp ./cmd/ps4-recomp)
# Prints each program's output as before and ends with one verdict line per case and "diag_acceptance: PASS|FAIL";
# exit status is 0 iff every case behaved as specified:
#   hello_ps4     runs until the timeout (rc 124) and printed "Hello world!" and "Done."
#   ps5_hello_k   rc 0, output "fib=463"
#   crash1        rc != 0, "about to fail" but not "not reached", Signal 11, crash.txt in the run bundle (signal 11)
#   crash2        AVX2 VPERMQ: the lifter implements it since m1 (clean run, rc 0, "not reached" printed) OR it is reported
#                 as unsupported with a crash.txt; anything else (hang, silent exit, other crash) fails
#   crash3        rc != 0, "unresolved import" naming sceKernelAprCtrl, crash.txt (SIGABRT) naming the import
#   crash4        rc != 0, Signal 5 (guest ud2 -> trap), crash.txt signal 5
W=${W:-$(cd "$(dirname "$0")/.." && pwd)}
SDK=${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/testbed/bin/ps5-payload-sdk/extracted/ps5-payload-sdk
cd $W && mkdir -p build/ps5 runs
fail=0; verdicts=()
for c in 1 2 3 4; do
  PATH=/opt/homebrew/opt/llvm/bin:$PATH $SDK/bin/prospero-clang -O1 -fplt -DCASE=$c tests/diag/crash_tests.c -lkernel_sys -o build/ps5/crash$c.elf || exit 1
done
cp ${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/work/s2/ps5/hello_k/hello_k_plt.elf build/ps5/ps5_hello_k.elf
verdict() { # name ok(0/1) detail
  if [ "$2" = 0 ]; then verdicts+=("PASS  $1  $3"); else verdicts+=("FAIL  $1  $3"); fail=1; fi
}
bundle_crash() { # name -> newest run bundle of that title that has a crash.txt
  local d; d=$(ls -dt runs/*-"$1"-run 2>/dev/null | head -1); [ -n "$d" ] && [ -s "$d/crash.txt" ] && echo "$d/crash.txt"
}
run() { # elf name [max lines]
  rm -rf "build/$2"; tools/build_ps5.sh "$1" "$2" 2>&1 | tail -1
  tools/run_sandboxed.sh 6 "build/$2/native" > "build/$2.diag.out" 2>&1; RC=$?
  grep -v '^\[ps4' "build/$2.diag.out" | head -${3:-6}
}
has() { grep -q -- "$1" "build/$2.diag.out"; }
HELLO_PS4="${NPS5_TEST_HELLO_ELF:-hello_world.elf}"   # upstream PS4Recomp sample, not committed here
if [ -f "$HELLO_PS4" ]; then run "$HELLO_PS4" hello_ps4
{ [ $RC = 124 ] && has "Hello world!" hello_ps4 && has "Done." hello_ps4; }; verdict hello_ps4 $? "rc=$RC"
else echo "SKIP hello_ps4: $HELLO_PS4 not present"; fi
run build/ps5/ps5_hello_k.elf ps5_hello_k
{ [ $RC = 0 ] && has "fib=463" ps5_hello_k; }; verdict ps5_hello_k $? "rc=$RC"
for c in 1 2 3 4; do
  n=crash$c; run build/ps5/crash$c.elf $n 3
  CT=$(bundle_crash $n)
  case $c in
  1) { [ $RC != 0 ] && has "about to fail" $n && ! has "not reached" $n && has "Signal 11" $n && [ -n "$CT" ] && grep -q '^signal: 11' "$CT"; }; verdict $n $? "rc=$RC ${CT:+crash.txt}" ;;
  2) { { [ $RC = 0 ] && has "not reached" $n; } || { [ $RC != 0 ] && ! has "not reached" $n && [ -n "$CT" ] && grep -qi 'unsupported' "$CT"; }; }; verdict $n $? "rc=$RC ${CT:+crash.txt}" ;;
  3) { [ $RC != 0 ] && has "unresolved import" $n && has "sceKernelAprCtrl" $n && [ -n "$CT" ] && grep -q 'sceKernelAprCtrl' "$CT"; }; verdict $n $? "rc=$RC ${CT:+crash.txt}" ;;
  4) { [ $RC != 0 ] && has "about to fail" $n && has "Signal 5" $n && [ -n "$CT" ] && grep -q '^signal: 5' "$CT"; }; verdict $n $? "rc=$RC ${CT:+crash.txt}" ;;
  esac
done
printf '%s\n' "${verdicts[@]}"
[ $fail = 0 ] && echo "diag_acceptance: PASS" || echo "diag_acceptance: FAIL"
exit $fail
