#!/usr/bin/env python3
"""Talk to QEMU QMP over a unix socket file.

Usage:
    python qmp-unix.py <sock> screendump <ppm-path>
    python qmp-unix.py <sock> key <name>            # hold+release
    python qmp-unix.py <sock> keydown <name>
    python qmp-unix.py <sock> keyup <name>
    python qmp-unix.py <sock> mouse_move <x> <y>
    python qmp-unix.py <sock> mouse_click
    python qmp-unix.py <sock> mouse_down
    python qmp-unix.py <sock> mouse_up
    python qmp-unix.py <sock> cpu <field>           # e.g. rip, eflags
"""
import json
import socket
import sys
import time

if sys.platform == "win32":
    import win32file  # type: ignore
    from pywintypes import OVERLAPPED  # type: ignore
    import win32event  # type: ignore
    import winerror  # type: ignore

    def connect_unix(path):
        handle = win32file.CreateFile(
            path,
            win32file.GENERIC_READ | win32file.GENERIC_WRITE,
            0,
            None,
            win32file.OPEN_EXISTING,
            win32file.FILE_FLAG_OVERLAPPED,
            None,
        )
        return handle

    def sock_send(handle, data):
        ol = OVERLAPPED()
        ol.hEvent = win32event.CreateEvent(None, 1, 0, None)
        hr, _ = win32file.WriteFile(handle, data, ol)
        if hr == winerror.ERROR_IO_PENDING:
            win32event.WaitForSingleObject(ol.hEvent, 5000)
            hr, _ = win32file.GetOverlappedResult(handle, ol, 1)
        if hr != 0:
            raise OSError(hr)

    def sock_recv_line(handle):
        buf = b""
        for _ in range(1000):
            ol = OVERLAPPED()
            ol.hEvent = win32event.CreateEvent(None, 1, 0, None)
            hr, data = win32file.ReadFile(handle, 4096, ol)
            if hr == winerror.ERROR_IO_PENDING:
                win32event.WaitForSingleObject(ol.hEvent, 5000)
                hr, data = win32file.GetOverlappedResult(handle, ol, 1)
            if hr == 0 and data:
                buf += data
            elif hr != 0 and hr != winerror.ERROR_MORE_DATA:
                break
            if b"\r\n" in buf:
                break
            time.sleep(0.02)
        return buf

else:

    def connect_unix(path):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(path)
        return s

    def sock_send(s, data):
        s.sendall(data)

    def sock_recv_line(s):
        buf = b""
        while b"\r\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
            if len(buf) > 65536:
                break
        return buf


def qmp_cmd(handle, cmd):
    payload = json.dumps(cmd).encode() + b"\r\n"
    sock_send(handle, payload)
    for _ in range(100):
        line = sock_recv_line(handle)
        try:
            obj = json.loads(line.decode(errors="replace").strip())
        except Exception:
            continue
        if obj.get("return") is not None or "error" in obj:
            return obj
    return None


def main():
    sock = sys.argv[1]
    op = sys.argv[2]
    handle = connect_unix(sock)
    qmp_cmd(handle, {"execute": "qmp_capabilities"})
    if op == "screendump":
        path = sys.argv[3]
        r = qmp_cmd(handle, {"execute": "screendump", "arguments": {"filename": path}})
        print(json.dumps(r))
    elif op == "key":
        qmp_cmd(handle, {"execute": "input-send-event",
                         "arguments": {"events": [
                             {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": sys.argv[3]}}}]}})
        qmp_cmd(handle, {"execute": "input-send-event",
                         "arguments": {"events": [
                             {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": sys.argv[3]}}}]}})
        print("ok")
    elif op in ("keydown", "keyup"):
        qmp_cmd(handle, {"execute": "input-send-event",
                         "arguments": {"events": [
                             {"type": "key", "data": {"down": op == "keydown", "key": {"type": "qcode", "data": sys.argv[3]}}}]}})
        print("ok")
    elif op == "mouse_move":
        qmp_cmd(handle, {"execute": "input-send-event",
                         "arguments": {"events": [
                             {"type": "abs", "data": {"axis": "x", "value": int(sys.argv[3])}},
                             {"type": "abs", "data": {"axis": "y", "value": int(sys.argv[4])}}]}})
        print("ok")
    elif op == "mouse_click":
        qmp_cmd(handle, {"execute": "input-send-event",
                         "arguments": {"events": [
                             {"type": "btn", "data": {"down": True, "button": "left"}},
                             {"type": "btn", "data": {"down": False, "button": "left"}}]}})
        print("ok")
    elif op == "mouse_down":
        qmp_cmd(handle, {"execute": "input-send-event",
                         "arguments": {"events": [
                             {"type": "btn", "data": {"down": True, "button": "left"}}]}})
        print("ok")
    elif op == "mouse_up":
        qmp_cmd(handle, {"execute": "input-send-event",
                         "arguments": {"events": [
                             {"type": "btn", "data": {"down": False, "button": "left"}}]}})
        print("ok")
    elif op == "cpu":
        r = qmp_cmd(handle, {"execute": "human-monitor-command",
                             "arguments": {"command-line": "info registers"}})
        if r and "return" in r:
            for line in str(r["return"]).splitlines():
                if sys.argv[3] in line:
                    print(line.strip())
        else:
            print(json.dumps(r))
    else:
        print("unknown op")


if __name__ == "__main__":
    main()
