#!/usr/bin/env bash
# Runs `go test` for the named tests of one package and fails unless every name was listed on this runner AND passed (a name
# that is missing, build-tagged away, skipped or "no tests to run" is a failure, not a green run).
#   require_tests.sh ./pkg/runtime TestKytyRouteTable [TestOther ...]
set -euo pipefail
pkg=${1:?package}
shift
[ $# -gt 0 ] || { echo "no test names" >&2; exit 2; }
listed=$(go test "$pkg" -list '.*' -count=1)
for t in "$@"; do
  printf '%s\n' "$listed" | grep -qx "$t" || { echo "::error::test $t is not built on this runner (build tag or file suffix?)" >&2; exit 1; }
done
re=$(printf '%s|' "$@")
out=$(go test "$pkg" -run "^(${re%|})\$" -count=1 -v)
printf '%s\n' "$out"
for t in "$@"; do
  printf '%s\n' "$out" | grep -q -- "^--- PASS: $t " || { echo "::error::test $t did not pass (skipped or missing)" >&2; exit 1; }
done
