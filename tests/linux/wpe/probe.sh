#!/bin/sh
# Runs Surfer on WebKit with a page probe: every 60 s the script prints what
# the page's thumbnails are doing (loaded, size, opacity, animations).
export WPE_CONSOLE=1 WPE_EVAL_EVERY=60
export WPE_EVAL='(() => { const ic = [...document.querySelectorAll("yt-icon, yt-icon-shape, ytd-logo, .yt-spec-icon-shape")];
  return JSON.stringify({ n: ic.length, s: ic.slice(0, 8).map((e) => { const root = e.shadowRoot || e, svg = root.querySelector("svg"), r = e.getBoundingClientRect(); return [e.localName, r.width | 0, !!e.shadowRoot, !!svg, svg ? svg.getBoundingClientRect().width | 0 : -1, svg ? svg.outerHTML.slice(0, 160) : (root.innerHTML || "").slice(0, 120), (e.getAttribute("icon") || "").slice(0, 30)]; }) }); })()'
exec /volumes/fat32-3/linux/usr/bin/icda-webkit "${1:-https://www.youtube.com/results?search_query=me+at+the+zoo}" > /volumes/fat32-3/wk.log 2>&1
