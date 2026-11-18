#!/bin/bash
# Privacy / identity gate for the commits of a range (default: origin/main..HEAD).
# 1. author and committer e-mails must be GitHub noreply addresses (*@users.noreply.github.com).
# 2. no added line may hold a home-directory path of a real user (/Users/<name>/, /home/<name>/, C:\Users\<name>\)
#    or an e-mail address other than a noreply / example one.
# Placeholders (/Users/you, /Users/<...>, /home/user, $HOME ...) and the lines of this script are allowed.
# This is the generic layer; scripts/check.sh --range does the file-type, secret and game-data checks.
# Usage: scripts/ci/check_identity.sh [A..B]     Exit 1 on a finding.
set -u
range="${1:-origin/main..HEAD}"
cd "$(git rev-parse --show-toplevel)" || exit 2
rc=0
bad=$(git log --format='%h %ae %ce' "$range" | awk '{for(i=2;i<=3;i++) if ($i !~ /@users\.noreply\.github\.com$/) {print $1, $i; break}}')
if [ -n "$bad" ]; then echo "identity: commit e-mail is not a GitHub noreply address:"; echo "$bad" | sed 's/^/  /'; rc=1; fi
PLACE='(you|Library|your|user|username|name|me|example|someone|foo|bar|test|runner|nobody|shared|Shared|Public|Guest|<[^>]*>|\$[A-Za-z{]|\*|X+|x+|\.\.\.)'
hits=$(git log -p --no-merges --format='commit %h' --no-color -U0 "$range" -- . ':!*check_identity.sh' ':!.github/workflows' |
  awk '/^commit /{c=$2} /^\+\+\+ /{f=substr($0,7)} /^\+[^+]/{print c "\t" f "\t" substr($0,2)}' |
  grep -E "(/Users/[A-Za-z0-9._-]+/|/home/[A-Za-z0-9._-]+/|[A-Za-z]:\\\\Users\\\\[A-Za-z0-9._-]+|[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})" |
  grep -vE "/Users/${PLACE}(/|[^A-Za-z0-9._-])|/home/${PLACE}(/|[^A-Za-z0-9._-])|@users\.noreply\.github\.com|@(example\.(com|org|net)|localhost)|@[A-Za-z0-9.-]+\.(invalid|example)\b|git@github\.com|noreply@(anthropic\.com|github\.com)|[A-Za-z0-9._%+-]+@[0-9]" || true)
# e-mail-looking text that is not an address: decorators and scoped packages (@types/node), Mach-O style names
hits=$(printf '%s\n' "$hits" | grep -vE "^\s*$" | grep -vE "[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.(png|jpg|svg|so|dylib|a|h|c|cc|cpp|py|sh|js|ts|go)\b" || true)
if [ -n "$hits" ]; then echo "identity: personal path or e-mail in added lines (commit, file, line):"; echo "$hits" | cut -c1-200 | sed 's/^/  /'; rc=1; fi
[ $rc = 0 ] && echo "identity: ok ($range)"
exit $rc
