#!/bin/bash
# The privacy and identity gate of CI: game data, secrets, personal paths (nps5_check.py) and commit e-mails / added
# lines (check_identity.sh) for the commits between $BASE and $TIP (scripts/ci/range.sh); the whole history when BASE is empty.
# Runs on the checkout it is in, so a component repository can carry a copy of scripts/nps5_check.py and scripts/ci/*.sh
# (see .github/nps5-ci/ there) and needs nothing from another repository.
# Usage: BASE=<commit or empty> TIP=<commit> privacy.sh [path of nps5_check.py [path of check_identity.sh]]
set -eu
d=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
chk="${1:-$d/nps5_check.py}"; [ -f "$chk" ] || chk="$d/../nps5_check.py"; idn="${2:-$d/check_identity.sh}"
if [ -n "${BASE:-}" ]; then
  python3 -I "$chk" --range "$BASE..$TIP"
  "$idn" "$BASE..$TIP"
else
  echo "no base commit (first push, new branch or manual run): checking the whole history"
  python3 -I "$chk" --history
  "$idn" "${TIP:-HEAD}"
fi
