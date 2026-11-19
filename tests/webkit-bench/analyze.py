#!/usr/bin/env python3
"""analyze.py CAMPAIGN_DIR [OUT.json]: medians of the campaign's runs, before and after.

Prints a table per scenario and writes the summary as JSON (for the report).
"""
import glob, json, os, re, statistics, sys


def med(xs):
    xs = [x for x in xs if x is not None]
    return round(statistics.median(xs), 3) if xs else None


def rate(ph, key):
    return ph[key] / ph["wall_s"] if ph and ph.get("wall_s") else None


def single_metrics(r):
    m = {"load_s": r.get("t_finished"), "first_paint_s": r.get("t_first_picture"),
         "load_timed_out": r.get("load_timed_out"), "crashes": r.get("crashes", 0)}
    for ph in ("load", "idle", "scroll"):
        d = r.get(ph)
        if not d:
            continue
        m[f"{ph}_cores"] = rate(d, "cpu_ns") / 1e9
        m[f"{ph}_cpu_s"] = d["cpu_ns"] / 1e9
        m[f"{ph}_syscalls_s"] = rate(d, "syscalls") if "syscalls" in d else None
        m[f"{ph}_faults_s"] = rate(d, "faults")
        m[f"{ph}_cswitch_s"] = rate(d, "cswitch")
        m[f"{ph}_frames_s"] = rate(d, "frames")
        m[f"{ph}_updates_s"] = rate(d, "flushes")
        m[f"{ph}_pss_max_mb"] = d.get("pss_max_kb", 0) / 1024
        for k, v in (d.get("cores_by_kind") or {}).items():
            m[f"{ph}_cores_{k}"] = v
    return m


def tabs_metrics(r):
    m = {"crashes": r.get("crashes", 0), "oom_kills": r.get("oom_kills"), "discards": r.get("discards", 0),
         "first_tab_reloaded_s": r.get("first_tab_reloaded_s"), "alive": r.get("alive")}
    d = r.get("idle")
    if d:
        m["idle_cores"] = rate(d, "cpu_ns") / 1e9
        m["idle_syscalls_s"] = rate(d, "syscalls") if "syscalls" in d else None
        m["idle_faults_s"] = rate(d, "faults")
        m["idle_frames_s"] = rate(d, "frames")
        m["idle_pss_end_mb"] = d.get("pss_end_kb", 0) / 1024
        for k, v in (d.get("cores_by_kind") or {}).items():
            m[f"idle_cores_{k}"] = v
    a = r.get("all")
    if a:
        m["pss_max_mb"] = a.get("pss_max_kb", 0) / 1024
    loads = [t.get("load_s") for t in r.get("tabs", [])]
    m["tabs_loaded"] = sum(1 for x in loads if x is not None)
    m["tabs_total"] = len(loads)
    m["tab_load_s"] = loads
    return m


def navigate_metrics(r):
    m = {"crashes": r.get("crashes", 0), "steps": [(s["url"], s.get("web_procs"), round((s.get("pss_kb") or 0) / 1024))
                                                    for s in r.get("steps", [])]}
    d = r.get("idle")
    if d:
        m["end_pss_mb"] = d.get("pss_end_kb", 0) / 1024
        m["end_web_procs"] = d.get("web_procs_max")
    m["step_load_s"] = [s.get("load_s") for s in r.get("steps", [])]
    return m


def main():
    root = sys.argv[1]
    runs = {}
    for f in sorted(glob.glob(os.path.join(root, "*.json"))):
        m = re.match(r"(.+)-(before|after)-r(\d+)\.json$", os.path.basename(f))
        if not m:
            continue
        r = json.load(open(f))
        name, build = m.group(1), m.group(2)
        if "tabs" in r and "urls" in r:
            met = tabs_metrics(r)
        elif "steps" in r:
            met = navigate_metrics(r)
        else:
            met = single_metrics(r)
        runs.setdefault(name, {}).setdefault(build, []).append(met)
    summary = {}
    for name, builds in runs.items():
        summary[name] = {}
        for build, ms in builds.items():
            keys = set().union(*ms)
            agg = {"runs": len(ms)}
            for k in keys:
                vals = [m.get(k) for m in ms]
                if all(isinstance(v, (int, float)) or v is None for v in vals) and not all(isinstance(v, bool) for v in vals if v is not None):
                    agg[k] = med(vals)
                else:
                    agg[k] = vals
            summary[name][build] = agg
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "summary.json")
    with open(out, "w") as f:
        json.dump(summary, f, indent=1)
    for name in sorted(summary):
        b, a = summary[name].get("before", {}), summary[name].get("after", {})
        print(f"\n== {name}  (runs: before {b.get('runs')}, after {a.get('runs')})")
        for k in sorted(set(b) | set(a)):
            if k == "runs":
                continue
            bv, av = b.get(k), a.get(k)
            if isinstance(bv, (int, float)) and isinstance(av, (int, float)) and not isinstance(bv, bool):
                ch = f"{(av - bv) / bv * 100:+.0f}%" if bv else ""
                print(f"   {k:28s} {bv:12.3f} {av:12.3f}  {ch}")
            else:
                print(f"   {k:28s} {str(bv)[:60]:60s} | {str(av)[:60]}")


if __name__ == "__main__":
    main()
