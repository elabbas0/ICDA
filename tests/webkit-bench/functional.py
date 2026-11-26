#!/usr/bin/env python3
"""Surfer's browser functions, driven as a person drives them, through the
emulated window manager (host/icda_sys.h): the address bar, a click and
typing into a page, Back and Forward, a new tab, switching and closing tabs,
reloading, a link that opens a new tab.  After each step the active tab's page is asked (WPE_EVAL, once
a second) for its title and address until it is what the step should give.

  functional.py --bin B [--out R.json]          (in the benchmark container)

Prints PASS or FAIL for each step; exits 1 if any failed.
"""
import argparse, json, os, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench

PORT = 18081
BASE = f"http://127.0.0.1:{PORT}"
ESC = "\x1b"
STEPS = [
    # (what, commands for the window manager, the title it should give)
    ("the first page loads", [], "Static page"),
    ("Ctrl+L, an address, Enter", ["key 12", f"type {BASE}/form.html", "key 13"], "Form"),
    ("a click in a text field, then typing", ["click 120 112", "type hello surfer"], "Form: hello surfer"),
    ("Alt+Left goes back", [f"type {ESC}[1;3D"], "Static page"),
    # with 8 GB (ram_mb below) the page is kept when left (the back-forward
    # cache), so it comes back as it was: what was typed still in its title
    ("Alt+Right goes forward", [f"type {ESC}[1;3C"], "Form: hello surfer"),
    ("Ctrl+T, an address, Enter: a new tab", [f"open {BASE}/ticker.html"], "Ticker"),
    ("Ctrl+PgUp: the tab before", [f"type {ESC}[5;5~"], "Form: hello surfer"),
    ("Ctrl+W closes it: the other tab shows", ["key 23"], "Ticker"),
    ("F5 loads the page again", [f"type {ESC}[15~"], "Ticker"),
    # a link opening a new tab: that page shares its opener's web process,
    # and gets Surfer's script for video and (drawing on the CPU) no WebGL
    ("a page with a link to a new tab", ["key 12", f"type {BASE}/opener.html", "key 13"], "Opener"),
    ("the link: the new tab gets the script, no WebGL", ["click 300 300"], "Probe: script true, webgl false"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--out")
    ap.add_argument("--step-timeout", type=float, default=20)
    a = ap.parse_args()
    pages = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pages")
    server = subprocess.Popen([sys.executable, "-m", "http.server", str(PORT), "--bind", "127.0.0.1"], cwd=pages,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    args = argparse.Namespace(bin=a.bin, url=f"{BASE}/static.html", urls=None, out=a.out, label="functional",
                              work=tempfile.mkdtemp(prefix="wkb-fn-"), win="1100x720", load_timeout=30, settle=1, idle=0,
                              scroll=0, scroll_rate=20, syscall_tp=0, proc=None, procemu="", ram_mb=8192, base_mb=700,
                              memstate=os.path.join(tempfile.gettempdir(), "wkb-fn-memstate"), enforce=False, shots=None, log=None,
                              env=["WPE_EVAL=document.title", "WPE_EVAL_EVERY=1"])
    results, failed = [], 0
    r = bench.Run(args, args.url)
    try:
        for what, cmds, want in STEPS:
            with r.lock:
                seen = len(r.lines)
            for c in cmds:
                r.command(c)
                time.sleep(0.3)
            got, end = None, time.monotonic() + a.step_timeout
            while time.monotonic() < end and r.proc.poll() is None:
                with r.lock:
                    evals = [l for _, l in r.lines[seen:] if l.startswith("icda-webkit eval: ")]
                if evals:
                    got = evals[-1][len("icda-webkit eval: "):]
                    if got == want:
                        break
                time.sleep(0.2)
            ok = got == want
            failed += not ok
            results.append({"step": what, "want": want, "got": got, "ok": ok})
            print(f"{'PASS' if ok else 'FAIL'} {what}" + ("" if ok else f" (title {got!r}, want {want!r})"), flush=True)
        alive = r.proc.poll() is None
        if not alive:
            failed += 1
            print("FAIL Surfer ended", flush=True)
    finally:
        r.stop()
        server.terminate()
    print(f"{len(STEPS) - failed if failed <= len(STEPS) else 0} of {len(STEPS)} steps passed, crashes {r.crashes}")
    if a.out:
        with open(a.out, "w") as f:
            json.dump({"steps": results, "crashes": r.crashes}, f, indent=1)
    sys.exit(1 if failed or r.crashes else 0)


if __name__ == "__main__":
    main()
