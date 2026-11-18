#!/bin/bash
# The commits a CI run looks at. Prints two lines for $GITHUB_ENV (or a shell `eval`):
#   BASE=<commit>      the revision the change starts from; EMPTY when there is none (first push, new branch, forced push,
#                      manual run): checks that only look at the lines a change touches are skipped then, the privacy
#                      gate checks the whole history (check.sh --history)
#   TIP=<commit>       the last commit of the change
# Input (environment): EVENT=<github.event_name>  PR_BASE, PR_HEAD (pull_request)  BEFORE (push: the old tip).
# Pull request: the PR's own base and head commits (not the merge commit). Push: the previous tip of the branch.
set -eu
zero=0000000000000000000000000000000000000000
base=""; tip=$(git rev-parse HEAD)
case "${EVENT:-}" in
  pull_request) base="${PR_BASE:-}"; tip="${PR_HEAD:-$tip}" ;;
  push) if [ -n "${BEFORE:-}" ] && [ "$BEFORE" != "$zero" ] && git cat-file -e "$BEFORE^{commit}" 2>/dev/null; then base=$BEFORE; fi ;;
esac
echo "BASE=$base"
echo "TIP=$tip"
