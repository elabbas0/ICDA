#!/bin/sh
# Opens a page in Surfer on WebKit with console messages on; every EVERY s
# prints the page's state (ready state, element count, text, what is drawn
# where, the styles that hide content).
[ -f /volumes/fat32-3/env.sh ] && . /volumes/fat32-3/env.sh
export WPE_CONSOLE=1 WPE_EVAL_EVERY=${EVERY:-20}
export WPE_EVAL='(() => { const cs = (e) => { if (!e) return null; const s = getComputedStyle(e), r = e.getBoundingClientRect();
    return [e.localName + (e.id ? "#" + e.id : "") + (e.className && e.className.baseVal === undefined ? "." + String(e.className).split(" ")[0] : ""),
      r.x | 0, r.y | 0, r.width | 0, r.height | 0, s.display, s.visibility, s.opacity, s.backgroundColor, s.color, s.transform.slice(0, 20), s.position]; };
  const pts = [[100, 50], [360, 150], [360, 250], [600, 30]].map(([x, y]) => cs(document.elementFromPoint(x, y)));
  return JSON.stringify({ rs: document.readyState, n: document.getElementsByTagName("*").length,
    txt: document.body ? document.body.innerText.slice(0, 120) : "", vw: innerWidth, vh: innerHeight,
    html: cs(document.documentElement), body: cs(document.body), pts, t: (performance.now() / 1000) | 0, url: location.href.slice(0, 60) }); })()'
B=/volumes/fat32-3/linux/bin/busybox
# PROF=a,b,c (or in env.sh next to this script): start the kernel profiler now, report after a, then b, then c seconds
if [ -n "$PROF" ]; then
  $B cat /dev/lxprof 2>/dev/null
  ( for s in ${PROF//,/ }; do $B sleep $s; $B cat /dev/lxprof 2>/dev/null; done ) &
fi
# TRACE=1: log every file a Linux program opens to the serial port
[ -n "$TRACE" ] && $B cat /dev/lxtrace 2>/dev/null
# DUMP=a,b: every Linux thread's state to the serial port after a, then b seconds
if [ -n "$DUMP" ]; then
  ( for s in ${DUMP//,/ }; do $B sleep $s; $B cat /dev/lxdump 2>/dev/null; done ) &
fi
# EVAL in env.sh: a probe of its own instead
[ -n "$EVAL" ] && export WPE_EVAL="$EVAL"
exec /volumes/fat32-3/linux/usr/bin/icda-webkit "${1:-https://www.google.com/}" > /volumes/fat32-3/wk.log 2>&1
