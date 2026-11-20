#!/usr/bin/env python3
"""Runs icda-webkit (Surfer on WebKit) on real pages and measures it.

The browser runs in a cgroup of its own; perf counts, for all its processes
(the browser, WPEWebProcess, WPENetworkProcess) and threads together:
CPU time, system calls, page faults and context switches.  On ICDA every
system call and page fault takes the kernel's one lock (kernel/cpu/smp.c), so
those counts are what the browser costs the rest of the machine there.
Memory is the sum of the processes' PSS.  Frames are counted by the emulated
window manager (host/icda_sys.h).

  bench.py single --bin B --url U --out R.json       load, idle, scroll
  bench.py tabs   --bin B --urls U1,U2,.. --out R.json   one tab each, then idle

Run inside the benchmark container (run.sh starts it).
"""
import argparse, ctypes, json, os, re, signal, struct, subprocess, sys, threading, time

# cgroup v2 (perf counts the browser's processes through it): hybrid hosts mount it at
# /sys/fs/cgroup/unified, pure v2 hosts at /sys/fs/cgroup
CG_ROOT = "/sys/fs/cgroup/unified" if os.path.isdir("/sys/fs/cgroup/unified") else "/sys/fs/cgroup"
# --enforce limits the browser to the emulated machine's RAM: cgroup v1's memory controller where
# there is one, else memory.max of the v2 group
MEMCG_ROOT = "/sys/fs/cgroup/memory" if os.path.isdir("/sys/fs/cgroup/memory") else None

# ---- perf counters for a cgroup ---------------------------------------------------------

class PerfAttr(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("size", ctypes.c_uint32), ("config", ctypes.c_uint64),
                ("sample_period", ctypes.c_uint64), ("sample_type", ctypes.c_uint64), ("read_format", ctypes.c_uint64),
                ("flags", ctypes.c_uint64), ("wakeup_events", ctypes.c_uint32), ("bp_type", ctypes.c_uint32),
                ("config1", ctypes.c_uint64), ("config2", ctypes.c_uint64), ("branch_sample_type", ctypes.c_uint64),
                ("sample_regs_user", ctypes.c_uint64), ("sample_stack_user", ctypes.c_uint32), ("clockid", ctypes.c_int32),
                ("sample_regs_intr", ctypes.c_uint64), ("aux_watermark", ctypes.c_uint32), ("sample_max_stack", ctypes.c_uint16),
                ("reserved2", ctypes.c_uint16), ("aux_sample_size", ctypes.c_uint32), ("reserved3", ctypes.c_uint32),
                ("sig_data", ctypes.c_uint64), ("config3", ctypes.c_uint64)]

libc = ctypes.CDLL(None, use_errno=True)
PERF_FLAG_PID_CGROUP = 4
EVENTS = {  # name: (type, config)
    "cpu_ns": (1, 1),        # software: task clock
    "faults": (1, 2),        # software: page faults
    "cswitch": (1, 3),       # software: context switches
}


class CgroupCounters:
    def __init__(self, cgdir, syscall_tp):
        self.fds = {}
        self.cgfd = os.open(cgdir, os.O_RDONLY)
        events = dict(EVENTS)
        if syscall_tp:
            events["syscalls"] = (2, syscall_tp)
        ncpu = os.cpu_count()
        for name, (typ, cfg) in events.items():
            fds = []
            for cpu in range(ncpu):
                a = PerfAttr()
                a.type, a.size, a.config = typ, ctypes.sizeof(PerfAttr), cfg
                fd = libc.syscall(298, ctypes.byref(a), self.cgfd, cpu, -1, PERF_FLAG_PID_CGROUP)
                if fd < 0:
                    err = ctypes.get_errno()
                    raise OSError(err, f"perf_event_open {name} cpu {cpu}: {os.strerror(err)}")
                fds.append(fd)
            self.fds[name] = fds

    def read(self):
        return {name: sum(struct.unpack("Q", os.read(fd, 8))[0] for fd in fds) for name, fds in self.fds.items()}

    def close(self):
        for fds in self.fds.values():
            for fd in fds:
                os.close(fd)
        os.close(self.cgfd)


# ---- processes ---------------------------------------------------------------------------

def kind_of(comm, cmdline):
    if "WebProcess" in comm or "WPEWebProcess" in cmdline:
        return "web"
    if "NetworkProc" in comm or "WPENetworkProcess" in cmdline:
        return "network"
    return "ui"


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("latin-1")
    except OSError:
        return ""


def process_detail(cgdir):
    """each process: kind, PSS (kB), threads by name"""
    out = []
    for pid in read(os.path.join(cgdir, "cgroup.procs")).split():
        comm = read(f"/proc/{pid}/comm").strip()
        roll = read(f"/proc/{pid}/smaps_rollup")
        m = re.search(r"^Pss:\s+(\d+) kB", roll, re.M)
        names = {}
        try:
            for tid in os.listdir(f"/proc/{pid}/task"):
                n = read(f"/proc/{pid}/task/{tid}/comm").strip()
                names[n] = names.get(n, 0) + 1
        except OSError:
            pass
        out.append({"pid": int(pid), "comm": comm, "pss_kb": int(m.group(1)) if m else 0, "threads": names})
    return out


def tcp_inodes():
    """inodes of the machine's TCP sockets that hold a connection slot (all but LISTEN)"""
    inodes = set()
    for f in ("/proc/net/tcp", "/proc/net/tcp6"):
        for line in read(f).splitlines()[1:]:
            parts = line.split()
            if len(parts) > 9 and parts[3] != "0A":
                inodes.add(parts[9])
    return inodes


def sample(cgdir):
    """per process: kind, pss/rss/anon (kB), threads, sockets (all kinds), tcp (connections)"""
    out = []
    tcp = tcp_inodes()
    for pid in read(os.path.join(cgdir, "cgroup.procs")).split():
        comm = read(f"/proc/{pid}/comm").strip()
        cmd = read(f"/proc/{pid}/cmdline").replace("\0", " ")
        roll = read(f"/proc/{pid}/smaps_rollup")
        if not roll:
            continue
        def field(name):
            m = re.search(r"^" + name + r":\s+(\d+) kB", roll, re.M)
            return int(m.group(1)) if m else 0
        st = read(f"/proc/{pid}/stat")
        fields = st.rsplit(")", 1)[1].split() if st else []
        threads = int(fields[17]) if fields else 0
        cpu_ticks = int(fields[11]) + int(fields[12]) if fields else 0      # utime + stime
        socks = conns = 0
        try:
            for fd in os.listdir(f"/proc/{pid}/fd"):
                try:
                    link = os.readlink(f"/proc/{pid}/fd/{fd}")
                except OSError:
                    continue
                if link.startswith("socket:"):
                    socks += 1
                    if link[8:-1] in tcp:
                        conns += 1
        except OSError:
            pass
        out.append({"pid": int(pid), "kind": kind_of(comm, cmd), "pss": field("Pss"), "rss": field("Rss"),
                    "anon": field("Anonymous"), "threads": threads, "sockets": socks, "tcp": conns, "cpu_ticks": cpu_ticks})
    return out


# ---- the run -------------------------------------------------------------------------------

class Run:
    def __init__(self, args, url):
        self.args = args
        self.t0 = time.monotonic()
        self.lines = []
        self.events = []           # (t, what, detail) from WPE_TIMING
        self.wm = None             # last benchwm statistics
        self.frames = 0            # frames the web processes handed over (icda-webkit frames lines)
        self.crashes = 0
        self.discards = 0
        self.notable = []          # (t, line): crashes and discards, whatever the log's length
        self.lock = threading.Lock()
        self.samples = []
        self.memstate = args.memstate
        tag = f"wkb-{os.getpid()}-{int(self.t0 * 1000) % 100000000}"
        self.cg = os.path.join(CG_ROOT, tag)
        os.mkdir(self.cg)
        self.memcg = None
        if args.enforce:
            # what the browser may use on the emulated machine: its RAM less the kernel and desktop;
            # past it the kernel ends a process, as ICDA's does when no page is left
            limit = str((args.ram_mb - args.base_mb) * 1024 * 1024)
            if MEMCG_ROOT:
                self.memcg = os.path.join(MEMCG_ROOT, tag)
                os.mkdir(self.memcg)
                with open(os.path.join(self.memcg, "memory.limit_in_bytes"), "w") as f:
                    f.write(limit)
            else:
                try:
                    with open(os.path.join(self.cg, "memory.max"), "w") as f:
                        f.write(limit)
                except OSError as e:
                    print(f"bench: cannot limit memory ({e}); running without --enforce", file=sys.stderr)
        self.counters = CgroupCounters(self.cg, args.syscall_tp)
        home = os.path.join(args.work, "home")
        subprocess.run(["rm", "-rf", args.work])
        os.makedirs(home)
        self.ctl = os.path.join(args.work, "ctl")
        open(self.ctl, "w").close()
        if self.memstate:
            with open(self.memstate, "w") as f:
                f.write("0\n")
        env = dict(os.environ)
        env.update({"HOME": home, "XDG_RUNTIME_DIR": home, "XDG_CACHE_HOME": home + "/.cache",
                    "XDG_DATA_HOME": home + "/.local/share", "WPE_TIMING": "1", "BENCH_CTL": self.ctl,
                    "BENCH_WIN": args.win})
        if args.proc:
            env.update({"BENCH_PROC": args.proc, "LD_PRELOAD": args.procemu, "BENCH_RAM_MB": str(args.ram_mb),
                        "BENCH_BASE_MB": str(args.base_mb), "BENCH_MEMSTATE": self.memstate})
        for kv in args.env or []:
            k, v = kv.split("=", 1)
            env[k] = v
        shim = f'echo $$ > {self.cg}/cgroup.procs && exec "$0" "$@"'
        if self.memcg:
            shim = f'echo $$ > {self.memcg}/cgroup.procs && ' + shim
        self.proc = subprocess.Popen(["sh", "-c", shim, args.bin, url], env=env, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, start_new_session=True)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.sampler_stop = False
        self.sampler = threading.Thread(target=self._sample, daemon=True)
        self.sampler.start()

    def now(self):
        return time.monotonic() - self.t0

    def _read(self):
        for raw in self.proc.stdout:
            line = raw.decode("utf-8", "replace").rstrip("\n")
            t = self.now()
            with self.lock:
                self.lines.append((round(t, 3), line[:300]))
                m = re.match(r"icda-webkit timing: (.+?) at ms (\d+) ?(.*)", line)
                if m:
                    self.events.append((t, m.group(1), m.group(3)[:200]))
                m = re.match(r"benchwm: ms=(\d+) flushes=(\d+) pixels=(\d+) comp_us=(\d+) events=(\d+)", line)
                if m:
                    self.wm = {"flushes": int(m.group(2)), "pixels": int(m.group(3)), "comp_us": int(m.group(4))}
                m = re.match(r"icda-webkit frames: (\d+) in", line)
                if m:
                    self.frames += int(m.group(1))
                if "web process crashed" in line or "web process used too much memory" in line:
                    self.crashes += 1
                    self.notable.append((round(t, 3), line[:200]))
                if "icda-webkit: discarded" in line:
                    self.discards += 1
                    self.notable.append((round(t, 3), line[:200]))

    def _sample(self):
        while not self.sampler_stop:
            procs = sample(self.cg)
            pss = sum(p["pss"] for p in procs)
            entry = {"t": round(self.now(), 2), "pss": pss, "rss": sum(p["rss"] for p in procs),
                     "threads": sum(p["threads"] for p in procs), "sockets": sum(p["sockets"] for p in procs),
                     "tcp": sum(p["tcp"] for p in procs), "nproc": len(procs),
                     "by_kind": {k: sum(p["pss"] for p in procs if p["kind"] == k) for k in ("ui", "web", "network")},
                     "web_procs": sum(1 for p in procs if p["kind"] == "web"),
                     "cpu_by_pid": {str(p["pid"]): [p["kind"], p["cpu_ticks"]] for p in procs}}
            with self.lock:
                self.samples.append(entry)
            if self.memstate:
                tmp = self.memstate + ".tmp"
                with open(tmp, "w") as f:
                    f.write(f"{pss}\n")
                os.replace(tmp, self.memstate)
            time.sleep(0.5)

    def snapshot(self):
        c = self.counters.read()
        cpu = read(os.path.join(self.cg, "cpu.stat"))
        def stat(k):
            m = re.search(r"^" + k + r" (\d+)", cpu, re.M)
            return int(m.group(1)) if m else 0
        with self.lock:
            wm = dict(self.wm or {"flushes": 0, "pixels": 0, "comp_us": 0})
            frames = self.frames
        return {"t": self.now(), **c, "user_us": stat("user_usec"), "system_us": stat("system_usec"),
                "usage_us": stat("usage_usec"), "wm": wm, "frames": frames}

    def command(self, text):
        with open(self.ctl, "a") as f:
            f.write(text + "\n")

    def shot(self, name):
        if self.args.shots:
            os.makedirs(self.args.shots, exist_ok=True)
            self.command(f"shot {os.path.join(self.args.shots, name)}.ppm")

    def wait_event(self, what, timeout, after=0.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            with self.lock:
                for t, w, _ in self.events:
                    if w == what and t >= after:
                        return t
            if self.proc.poll() is not None:
                return None
            time.sleep(0.05)
        return None

    def first_event(self, what, after=0.0):
        with self.lock:
            for t, w, _ in self.events:
                if w == what and t >= after:
                    return t
        return None

    def stop(self):
        self.sampler_stop = True
        try:
            with open(os.path.join(self.cg, "cgroup.kill"), "w") as f:
                f.write("1")
        except OSError:
            os.killpg(self.proc.pid, signal.SIGKILL)
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        self.sampler.join(timeout=5)
        self.counters.close()
        self.oom_kills = None
        if self.memcg:
            m = re.search(r"oom_kill (\d+)", read(os.path.join(self.memcg, "memory.oom_control")))
            self.oom_kills = int(m.group(1)) if m else None
        elif self.args.enforce:
            m = re.search(r"^oom_kill (\d+)", read(os.path.join(self.cg, "memory.events")), re.M)
            self.oom_kills = int(m.group(1)) if m else None
            for _ in range(50):
                try:
                    os.rmdir(self.memcg)
                    break
                except OSError:
                    time.sleep(0.1)
        for _ in range(50):
            try:
                os.rmdir(self.cg)
                break
            except OSError:
                time.sleep(0.1)


def phase(a, b, samples):
    d = {k: b[k] - a[k] for k in ("cpu_ns", "faults", "cswitch", "user_us", "system_us", "usage_us", "frames")}
    if "syscalls" in a:
        d["syscalls"] = b["syscalls"] - a["syscalls"]
    d["wall_s"] = b["t"] - a["t"]
    d["flushes"] = b["wm"]["flushes"] - a["wm"]["flushes"]
    d["wm_pixels"] = b["wm"]["pixels"] - a["wm"]["pixels"]
    d["wm_comp_us"] = b["wm"]["comp_us"] - a["wm"]["comp_us"]
    inside = [s for s in samples if a["t"] <= s["t"] <= b["t"]]
    if inside:
        d["pss_max_kb"] = max(s["pss"] for s in inside)
        d["pss_end_kb"] = inside[-1]["pss"]
        d["by_kind_end_kb"] = inside[-1]["by_kind"]
        d["threads_max"] = max(s["threads"] for s in inside)
        d["sockets_max"] = max(s["sockets"] for s in inside)
        d["tcp_max"] = max(s["tcp"] for s in inside)
        d["web_procs_max"] = max(s["web_procs"] for s in inside)
        first, last = inside[0]["cpu_by_pid"], inside[-1]["cpu_by_pid"]
        hz = os.sysconf("SC_CLK_TCK")
        span = max(inside[-1]["t"] - inside[0]["t"], 0.001)
        by = {"ui": 0.0, "web": 0.0, "network": 0.0}
        for pid, (kind, ticks) in last.items():
            by[kind] += (ticks - first.get(pid, [kind, 0])[1]) / hz / span
        d["cores_by_kind"] = {k: round(v, 3) for k, v in by.items()}
    return d


def single(args):
    r = Run(args, args.url)
    res = {"url": args.url, "bin": args.bin, "label": args.label}
    try:
        s0 = r.snapshot()
        t_fin = r.wait_event("finished", args.load_timeout)
        s1 = r.snapshot()
        res["load_timed_out"] = t_fin is None
        res["t_committed"] = r.first_event("committed")
        res["t_first_picture"] = r.first_event("first picture", after=res["t_committed"] or 0.0)
        res["t_finished"] = t_fin
        res["load"] = phase(s0, s1, r.samples)
        time.sleep(args.settle)
        r.shot("1-loaded")
        s2 = r.snapshot()
        time.sleep(args.idle)
        s3 = r.snapshot()
        res["idle"] = phase(s2, s3, r.samples)
        r.command(f"scroll {args.scroll} {args.scroll_rate}")
        time.sleep(args.scroll * 0.55)
        r.shot("2-scrolled")
        time.sleep(args.scroll * 0.45 + 0.5)
        s4 = r.snapshot()
        res["scroll"] = phase(s3, s4, r.samples)
        r.shot("3-end")
        time.sleep(0.3)
        res["alive"] = r.proc.poll() is None
    finally:
        r.stop()
    res["crashes"] = r.crashes
    res["oom_kills"] = r.oom_kills
    res["events"] = r.notable
    res["samples"] = r.samples
    res["log_tail"] = [l for l in r.lines if not l[1].startswith("benchwm:")][-60:]
    res["all_lines"] = r.lines
    return res


def tabs(args):
    urls = args.urls.split(",")
    r = Run(args, urls[0])
    res = {"urls": urls, "bin": args.bin, "label": args.label, "tabs": []}
    try:
        start = r.snapshot()
        prev = start
        for i, u in enumerate(urls):
            t_req = r.now()
            if i:
                r.command(f"open {u}")
            t_fin = r.wait_event("finished", args.load_timeout, after=t_req)
            time.sleep(args.settle)
            s = r.snapshot()
            res["tabs"].append({"url": u, "load_s": (t_fin - t_req) if t_fin else None, "phase": phase(prev, s, r.samples)})
            prev = s
        time.sleep(args.idle)
        end = r.snapshot()
        res["idle"] = phase(prev, end, r.samples)
        res["all"] = phase(start, end, r.samples)
        # back to the first tab, as a user would: how long until it is drawn again
        x = 6 + (min((int(args.win.split("x")[0]) - 44) // len(urls), 220)) // 2
        r.shot("1-all-tabs-open")
        t_click = r.now()
        r.command(f"click {x} 20")
        time.sleep(3)
        r.shot("2-first-tab-after-3s")
        t_back = r.wait_event("finished", 30, after=t_click)
        time.sleep(2)
        r.shot("3-first-tab-later")
        res["first_tab_reloaded_s"] = (t_back - t_click) if t_back else None
        res["alive"] = r.proc.poll() is None
    finally:
        r.stop()
    res["crashes"] = r.crashes
    res["discards"] = r.discards
    res["oom_kills"] = r.oom_kills
    res["events"] = r.notable
    res["samples"] = r.samples
    res["log_tail"] = [l for l in r.lines if not l[1].startswith("benchwm:")][-80:]
    res["all_lines"] = r.lines
    return res


def navigate(args):
    """one tab, the sites one after another through the address bar (Ctrl+L, the address, Enter)"""
    urls = args.urls.split(",")
    r = Run(args, urls[0])
    res = {"urls": urls, "bin": args.bin, "label": args.label, "steps": []}
    try:
        start = r.snapshot()
        prev = start
        for i, u in enumerate(urls):
            t_req = r.now()
            if i:
                r.command("key 12")
                r.command("type " + u)
                r.command("key 13")
            t_fin = r.wait_event("finished", args.load_timeout, after=t_req)
            time.sleep(args.settle)
            s = r.snapshot()
            with r.lock:
                last = r.samples[-1] if r.samples else {}
            res["steps"].append({"url": u, "load_s": (t_fin - t_req) if t_fin else None, "phase": phase(prev, s, r.samples),
                                 "web_procs": last.get("web_procs"), "pss_kb": last.get("pss")})
            prev = s
        time.sleep(args.idle)
        end = r.snapshot()
        res["idle"] = phase(prev, end, r.samples)
        res["all"] = phase(start, end, r.samples)
        res["processes"] = process_detail(r.cg)
        res["alive"] = r.proc.poll() is None
    finally:
        r.stop()
    res["crashes"] = r.crashes
    res["oom_kills"] = r.oom_kills
    res["events"] = r.notable
    res["samples"] = r.samples
    res["log_tail"] = [l for l in r.lines if not l[1].startswith("benchwm:")][-80:]
    res["all_lines"] = r.lines
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["single", "tabs", "navigate"])
    ap.add_argument("--bin", required=True)
    ap.add_argument("--url")
    ap.add_argument("--urls")
    ap.add_argument("--out", required=True)
    ap.add_argument("--label", default="")
    ap.add_argument("--work", default="/tmp/wkb-run")
    ap.add_argument("--win", default="1100x720")
    ap.add_argument("--load-timeout", type=float, default=45)
    ap.add_argument("--settle", type=float, default=3)
    ap.add_argument("--idle", type=float, default=15)
    ap.add_argument("--scroll", type=float, default=10)
    ap.add_argument("--scroll-rate", type=float, default=20, help="wheel notches a second while scrolling")
    ap.add_argument("--syscall-tp", type=int, default=int(os.environ.get("SYSCALL_TP", "0")))
    ap.add_argument("--proc", choices=["icda-old", "icda-new"])
    ap.add_argument("--procemu", default="/bench/out/procemu.so")
    ap.add_argument("--ram-mb", type=int, default=4096)
    ap.add_argument("--base-mb", type=int, default=700)
    ap.add_argument("--memstate", default="/tmp/wkb-memstate")
    ap.add_argument("--enforce", action="store_true", help="limit the browser to --ram-mb less --base-mb (cgroup v1)")
    ap.add_argument("--env", action="append")
    ap.add_argument("--shots", help="directory for screenshots (PPM) at the end of each phase")
    ap.add_argument("--log", help="file for everything the browser printed")
    args = ap.parse_args()
    res = {"single": single, "tabs": tabs, "navigate": navigate}[args.mode](args)
    if args.log:
        with open(args.log, "w") as f:
            for t, line in res.pop("all_lines", []):
                f.write(f"{t:9.3f} {line}\n")
    res.pop("all_lines", None)
    with open(args.out, "w") as f:
        json.dump(res, f, indent=1)


if __name__ == "__main__":
    main()
