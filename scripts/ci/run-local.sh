#!/usr/bin/env bash
# Run the jobs of .github/workflows/ci.yml locally, from a clean checkout of HEAD, before pushing.
#
# It executes the very `run:` steps of the workflow (no second copy of the commands to drift), in a scratch
# `git worktree` of HEAD (no submodules, untracked and uncommitted files absent), with a fresh Python venv for
# `setup-python` jobs, the runner's bash options (-e -o pipefail) and the same env/GITHUB_ENV plumbing.
#
#   scripts/ci/run-local.sh              all jobs
#   scripts/ci/run-local.sh --job go     one job (the id under `jobs:`)
#   scripts/ci/run-local.sh --x86        run macos-*-intel jobs under Rosetta (`arch -x86_64`), like the hosted runner
#   scripts/ci/run-local.sh --keep       keep the scratch checkout (path is printed)
#   scripts/ci/run-local.sh --act        also run the ubuntu jobs in nektos/act (needs `brew install act` and Docker)
#
# Commit first: only committed work is tested. Needs ruby (macOS has it), git, and the tools the jobs use
# (shellcheck, go, cmake, ninja, ... see docs/ci.md). Not reproducible here: the hosted runners' image and queue.
set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"
job="" x86=0 keep=0 act=0
while [ $# -gt 0 ]; do
  case "$1" in
    --job) job=${2:?}; shift ;;
    --x86) x86=1 ;;
    --keep) keep=1 ;;
    --act) act=1 ;;
    -h|--help) sed -n 2,14p "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
[ -z "$(git status --porcelain --untracked-files=no)" ] || echo "warning: uncommitted changes are NOT tested (the checkout is HEAD)" >&2

scratch=$(mktemp -d "${TMPDIR:-/tmp}/ci-local.XXXXXX")
cleanup() {
  [ "$keep" = 1 ] && { echo "kept: $scratch/work"; return; }
  git -C "$root" worktree remove --force "$scratch/work" >/dev/null 2>&1 || true
  rm -rf "$scratch"
}
trap cleanup EXIT
git worktree add --detach "$scratch/work" HEAD >/dev/null 2>&1
export LOCAL_CI_SCRATCH="$scratch" LOCAL_CI_X86="$x86" LOCAL_CI_JOB="$job" LOCAL_CI_ROOT="$root"

rc=0
ruby -ryaml -rfileutils -e '
scratch = ENV["LOCAL_CI_SCRATCH"]; work = "#{scratch}/work"; only = ENV["LOCAL_CI_JOB"]
wf = YAML.load_file("#{work}/.github/workflows/ci.yml")
head = `git -C #{work} rev-parse HEAD`.strip
base = `git -C #{work} merge-base HEAD origin/main 2>/dev/null || git -C #{work} merge-base HEAD main`.strip
exprs = { "github.event_name" => "pull_request", "github.event.pull_request.base.sha" => base,
          "github.event.pull_request.head.sha" => head, "github.event.before" => "0" * 40 }
subst = ->(s) { s.to_s.gsub(/\$\{\{\s*([^}]*?)\s*\}\}/) { exprs.fetch($1, "") } }
bash = `which -a bash`.split.find { |b| `#{b} -c "echo ${BASH_VERSINFO[0]}"`.strip.to_i >= 4 } || "bash" # the runner has bash 5 on Linux
darwin = RUBY_PLATFORM.include?("darwin")
results = []
wf["jobs"].each do |id, j|
  next if only != "" && only != id
  name = j["name"] || id
  mac = j["runs-on"].to_s.start_with?("macos")
  intel = j["runs-on"].to_s.include?("intel")
  if !darwin && mac then results << [name, "skipped (needs macOS)"]; next end
  env = (wf["env"] || {}).merge(j["env"] || {})
  tmp = "#{scratch}/tmp-#{id}"; FileUtils.mkdir_p(tmp)
  genv = "#{tmp}/github_env"; File.write(genv, "")
  path = ENV["PATH"]; ok = true
  puts "\n=== #{name} (#{id}, #{j["runs-on"]}) ==="
  (j["steps"] || []).each_with_index do |st, i|
    if st["uses"].to_s.start_with?("actions/setup-python")
      py = `command -v python#{st.dig("with","python-version")} 2>/dev/null`.strip
      py = `command -v python3`.strip if py.empty?
      system(py, "-m", "venv", "#{tmp}/venv") or abort("venv failed")
      path = "#{tmp}/venv/bin:#{path}"
      puts "-- setup-python: fresh venv from #{`#{py} --version`.strip}"
    elsif st["uses"].to_s.start_with?("actions/setup-go")
      want = File.read("#{work}/go.mod")[/^go (\S+)/, 1]
      puts "-- setup-go: go.mod wants #{want}; local #{`go version`.strip}"
    end
    next unless st["run"]
    cmd = st["run"]
    if darwin && cmd.include?("actionlint_1.7.7_linux_amd64")
      cmd = "command -v actionlint >/dev/null || { echo \"install actionlint (brew install actionlint)\"; exit 1; }\nactionlint # on Linux CI: pinned 1.7.7 tarball, sha256 checked; local: #{`actionlint -version 2>/dev/null | head -1`.strip}"
    end
    senv = ENV.to_h.merge(env).merge(st["env"] || {}).transform_values { |v| subst.(v) }
    File.readlines(genv, chomp: true).each { |l| k, v = l.split("=", 2); senv[k] = v if v }
    senv.merge!("PATH" => path, "CI" => "true", "GITHUB_ACTIONS" => "true", "GITHUB_ENV" => genv, "GITHUB_WORKSPACE" => work,
                "RUNNER_TEMP" => tmp, "GITHUB_EVENT_NAME" => "pull_request")
    script = "#{tmp}/step#{i}.sh"; File.write(script, subst.(cmd))
    sh = [mac ? "bash" : bash, "--noprofile", "--norc", "-e", "-o", "pipefail", script]
    sh = ["arch", "-x86_64"] + sh if mac && intel && ENV["LOCAL_CI_X86"] == "1"
    puts "-- #{st["name"] || cmd.lines.first.strip[0, 80]}"
    unless system(senv, *sh, chdir: work)
      puts "FAILED: #{st["name"] || cmd.lines.first.strip[0, 80]}"; ok = false; break
    end
  end
  results << [name, ok ? "pass" : "FAIL"]
end
puts "\n=== summary ==="; results.each { |n, r| puts "#{r.ljust(24)} #{n}" }
exit(results.any? { |_, r| r == "FAIL" } ? 1 : 0)
' || rc=$?

if [ "$act" = 1 ]; then
  command -v act >/dev/null || { echo "act not installed (brew install act); skipping --act" >&2; exit "$rc"; }
  for j in $(ruby -ryaml -e 'YAML.load_file(".github/workflows/ci.yml")["jobs"].each { |k, v| puts k if v["runs-on"].to_s.start_with?("ubuntu") }'); do
    [ -z "$job" ] || [ "$job" = "$j" ] || continue
    act pull_request -j "$j" --container-architecture linux/amd64 || rc=$?
  done
fi
exit "$rc"
