#!/bin/sh
# The Windows layer against a conformance test suite: Wine's test programs
# for each Windows DLL (winetest64.exe bundles them; Wine's CI publishes it
# daily), run under icda-win one test at a time, as Windows itself is
# expected to pass them.  Writes a scoreboard of each DLL's tests: passed,
# failed (and how many checks), crashed, or still running at the timeout.
#   tests/winlayer/conformance.sh <winetest64.exe> [DLL ...]   (default: the layer's DLLs)
#   TIMEOUT=60 JOBS=8 ICDA_WIN=userspace/winlayer/out/icda-win
# Results: tests/winlayer/out/conformance/{results.tsv,scoreboard.txt,logs/}
cd "$(dirname "$0")/../.."
BUNDLE=${1:?usage: $0 <winetest64.exe> [DLL ...]}
shift
DLLS=${*:-"kernel32 kernelbase ntdll msvcrt ucrtbase user32 gdi32 advapi32 shell32 shlwapi ole32 combase oleaut32 version ws2_32 winmm bcrypt cabinet dwrite imm32 setupapi opengl32 shcore"}
ICDA_WIN=${ICDA_WIN:-userspace/winlayer/out/icda-win}
[ -x "$ICDA_WIN" ] || sh userspace/winlayer/build.sh
O=tests/winlayer/out/conformance
mkdir -p $O
cp "$BUNDLE" $O/winetest64.exe
cp "$ICDA_WIN" $O/icda-win
MSYS_NO_PATHCONV=1 docker run --rm -e DLLS="$DLLS" -e TIMEOUT="${TIMEOUT:-60}" -e JOBS="${JOBS:-$(nproc)}" \
    -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w/$O alpine:3.20 sh -c '
set -e
apk add --no-cache mesa-osmesa libmspack freetype font-dejavu 7zip python3 >/dev/null
rm -rf tests logs && mkdir -p tests logs
7zz e -y -otests winetest64.exe ".rsrc/TESTRES/*" >/dev/null
export HOME=/tmp/h ICDA_WIN_HEADLESS=1 WINETEST_INTERACTIVE=0 WINETEST_REPORT_SUCCESS=0
: > todo.txt
for d in $DLLS; do
    exe=$(ls tests | grep -i "^${d}_test.exe$" | head -1)
    [ -n "$exe" ] || { echo "$d: no tests" >&2; continue; }
    mkdir -p "run/$d" && cp "tests/$exe" "run/$d/${d}_test.exe"
    # the test names: "Valid test names:" then one a line
    (cd "run/$d" && timeout 30 /w/'"$O"'/icda-win "${d}_test.exe" --list 2>/dev/null) |
        sed -n "/Valid test names/,\$p" | tail -n +2 | tr -d "\r" | awk "NF {print \"$d \" \$1}" >> todo.txt
done
echo "$(wc -l < todo.txt) tests:" $(awk "{print \$1}" todo.txt | uniq -c | awk "{print \$2 \"=\" \$1}")
# each test in a session of its own (a test that ends processes must not end
# the runner), killed whole at the timeout
python3 - <<PY
import os, signal, subprocess, concurrent.futures as cf
todo = [l.split() for l in open("todo.txt") if l.strip()]
here = os.getcwd()
def one(dt):
    d, t = dt
    with open("logs/%s-%s.txt" % (d, t), "wb") as log:
        p = subprocess.Popen([os.path.join(here, "icda-win"), "%s_test.exe" % d, t], cwd="run/" + d, stdout=log,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
        try:
            rc = p.wait(timeout=int(os.environ["TIMEOUT"]))
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            rc = 137
    return "%s %s %d" % (d, t, rc)
with cf.ThreadPoolExecutor(int(os.environ["JOBS"])) as ex, open("exits.txt", "w") as out:
    for r in ex.map(one, todo):
        out.write(r + "\n")
PY
python3 - <<PY
import re, os, collections
summ = re.compile(r"^[0-9a-f]+:(\S+): (\d+) tests? executed \((\d+) marked as todo, (?:(\d+) as flaky, )?(\d+) failures?\), (\d+) skipped")
rows = []
for line in open("exits.txt"):
    d, t, rc = line.split()
    rc = int(rc)
    log = open("logs/%s-%s.txt" % (d, t), errors="replace").read()
    m = [x for x in (summ.match(l) for l in log.splitlines()) if x]
    executed = sum(int(x.group(2)) for x in m); failures = sum(int(x.group(5)) for x in m)
    skipped = sum(int(x.group(6)) for x in m)
    if rc == 137: status = "timeout"
    elif not m: status = "crash"
    elif failures: status = "fail"
    elif rc: status = "crash"
    else: status = "pass"
    rows.append((d, t, status, executed, failures, skipped))
rows.sort()
with open("results.tsv", "w") as f:
    f.write("dll\ttest\tstatus\tchecks\tfailures\tskipped\n")
    for r in rows: f.write("\t".join(map(str, r)) + "\n")
by = collections.OrderedDict()
for d, t, s, e, fl, sk in rows:
    b = by.setdefault(d, collections.Counter())
    b[s] += 1; b["tests"] += 1; b["checks"] += e; b["failures"] += fl
tot = collections.Counter()
lines = ["%-12s %6s %6s %6s %6s %7s %9s %9s" % ("dll", "tests", "pass", "fail", "crash", "timeout", "checks", "failures")]
for d, b in by.items():
    lines.append("%-12s %6d %6d %6d %6d %7d %9d %9d" % (d, b["tests"], b["pass"], b["fail"], b["crash"], b["timeout"], b["checks"], b["failures"]))
    tot.update(b)
lines.append("%-12s %6d %6d %6d %6d %7d %9d %9d" % ("all", tot["tests"], tot["pass"], tot["fail"], tot["crash"], tot["timeout"], tot["checks"], tot["failures"]))
open("scoreboard.txt", "w").write("\n".join(lines) + "\n")
print("\n".join(lines))
PY
'
