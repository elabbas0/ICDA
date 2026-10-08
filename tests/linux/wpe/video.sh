#!/bin/sh
# Plays a YouTube video in Surfer on WebKit; every 30 s prints the video
# element's state (time, ready state, paused, size, decoded frames, error).
export WPE_CONSOLE=1 WPE_EVAL_EVERY=30 GST_DEBUG=2
export WPE_EVAL='(() => { const v = document.querySelector("video"); if (!v) return "no video";
  const q = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : {};
  return JSON.stringify({ t: v.currentTime.toFixed(1), rs: v.readyState, p: v.paused, w: v.videoWidth, h: v.videoHeight,
    frames: q.totalVideoFrames, dropped: q.droppedVideoFrames, err: v.error && v.error.code, src: v.currentSrc.slice(0, 30) }); })()'
exec /volumes/fat32-3/linux/usr/bin/icda-webkit "${1:-https://www.youtube.com/watch?v=jNQXAC9IVRw}" > /volumes/fat32-3/wk.log 2>&1
