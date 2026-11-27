#!/usr/bin/env python3
"""The before/after campaign: every scenario run with both builds, alternately.

  campaign.py OUTDIR [--reps N] [--only name,name]

Inside the benchmark container (run.sh).  The builds are
/bench/out/icda-webkit-before and -after; each runs with ICDA's /proc as its
kernel provides it (before: ICDA 1.10.3, after: with this change; see
host/procemu.c).  Results: OUTDIR/<scenario>-<build>-r<rep>.json.
"""
import argparse, os, shutil, subprocess, sys, tempfile, time

PAGES = "http://127.0.0.1:18080"
SITES = {
    "wikipedia": "https://en.wikipedia.org/wiki/Web_browser",
    "bbc": "https://www.bbc.com/news",
    "cnn": "https://www.cnn.com/",
    "verge": "https://www.theverge.com/",
    "espn": "https://www.espn.com/",
    "youtube": "https://www.youtube.com/results?search_query=web+browser+history",
    "nbcnews": "https://www.nbcnews.com/",
}
NAVIGATE = ["wikipedia", "bbc", "espn", "verge", "cnn", "youtube"]
TABS = ["wikipedia", "bbc", "espn", "verge", "cnn", "youtube", "nbcnews", "https://www.foxnews.com/"]


def url(name):
    return SITES.get(name, name)


def scenarios():
    s = []
    for page in ("anim-hidden", "anim-visible", "static", "raf-canvas", "video", "ticker", "mse"):
        s.append((f"local-{page}", ["single", "--url", f"{PAGES}/{page}.html", "--idle", "15", "--scroll", "6",
                                     "--load-timeout", "20", "--ram-mb", "8192"], 3))
    s.append(("local-background-tab", ["tabs", "--urls", f"{PAGES}/anim-hidden.html,{PAGES}/static.html", "--idle", "15",
                                       "--load-timeout", "20", "--ram-mb", "8192"], 3))
    s.append(("local-background-tab-compositor", ["tabs", "--urls", f"{PAGES}/anim-accel.html,{PAGES}/static.html", "--idle", "15",
                                                  "--load-timeout", "20", "--ram-mb", "8192"], 3))
    for name, u in SITES.items():
        s.append((f"site-{name}", ["single", "--url", u, "--idle", "15", "--scroll", "10", "--load-timeout", "45",
                                   "--ram-mb", "8192"], 3))
    s.append(("navigate-6-sites", ["navigate", "--urls", ",".join(url(n) for n in NAVIGATE), "--idle", "15",
                                   "--load-timeout", "45", "--ram-mb", "8192"], 2))
    s.append(("tabs-8-on-4GB", ["tabs", "--urls", ",".join(url(n) for n in TABS), "--idle", "20", "--load-timeout", "45",
                                "--ram-mb", "4096", "--enforce"], 2))
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--reps", type=int, help="overrides every scenario's repetitions")
    ap.add_argument("--only", help="comma-separated scenario names")
    ap.add_argument("--proc-before", default="icda-old",
                    help="the /proc the before build sees (icda-old: ICDA 1.10.3's; icda-new when the kernel is not compared)")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    # the pages, and a 30 frames-a-second test video made with GStreamer for pages/video.html
    pages = tempfile.mkdtemp(prefix="wkb-pages-")
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pages")
    for f in os.listdir(src):
        shutil.copy(os.path.join(src, f), pages)
    try:
        subprocess.run(["gst-launch-1.0", "-q", "videotestsrc", "num-buffers=300", "pattern=ball", "!",
                        "video/x-raw,width=640,height=360,framerate=30/1", "!", "vp8enc", "deadline=1", "!", "webmmux", "!",
                        "filesink", f"location={os.path.join(pages, 'test.webm')}"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # and for pages/mse.html: 720p VP9, 20 s, 8-bit 4:2:0 as on the web (without
        # format=I420 the encoder takes 12-bit 4:4:4, which WebKit 2.54 does not show)
        subprocess.run(["gst-launch-1.0", "-q", "videotestsrc", "num-buffers=600", "pattern=ball", "!",
                        "video/x-raw,format=I420,width=1280,height=720,framerate=30/1", "!", "vp9enc", "deadline=1", "cpu-used=8", "!",
                        "webmmux", "!", "filesink", f"location={os.path.join(pages, 'test-vp9.webm')}"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        print("campaign: no gst-launch-1.0 (gstreamer-tools): local-video and local-mse play nothing", file=sys.stderr)
    server = subprocess.Popen([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "rangeserve.py"), "18080", pages], cwd=pages,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    only = set(args.only.split(",")) if args.only else None
    try:
        for name, cmd, reps in scenarios():
            if only and name not in only:
                continue
            for rep in range(1, (args.reps or reps) + 1):
                order = ("before", "after") if rep % 2 else ("after", "before")
                for build in order:
                    out = os.path.join(args.out, f"{name}-{build}-r{rep}.json")
                    if os.path.exists(out):
                        continue
                    proc = args.proc_before if build == "before" else "icda-new"
                    full = [sys.executable, "/bench/bench.py", *cmd, "--bin", f"/bench/out/icda-webkit-{build}",
                            "--proc", proc, "--out", out, "--label", f"{name} {build} r{rep}"]
                    if rep == 1:
                        full += ["--shots", os.path.join(args.out, "shots", f"{name}-{build}")]
                    t0 = time.time()
                    rc = subprocess.run(full, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
                    print(f"{name:24s} {build:6s} r{rep}  {time.time() - t0:5.0f} s  {'ok' if rc == 0 else 'exit %d' % rc}",
                          flush=True)
    finally:
        server.terminate()


if __name__ == "__main__":
    main()
