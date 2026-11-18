# nps5-ci

A copy of the privacy and identity gate of the entry repository
([nativeps5](https://github.com/computerguy526/nativeps5): `scripts/nps5_check.py`, `scripts/ci/check_identity.sh`,
`range.sh`, `privacy.sh`), so this repository's CI needs nothing from another repository. Keep it in step with the
originals; do not edit it here. Run the same gate locally with `BASE=origin/main TIP=HEAD .github/nps5-ci/privacy.sh`.
