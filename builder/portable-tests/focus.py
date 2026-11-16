#!/usr/bin/env python3
"""Read X input focus for browser-test synchronization; never inject input."""
import ctypes as c
import json
from pathlib import Path
import time

x = c.CDLL('libX11.so.6')
x.XOpenDisplay.argtypes = [c.c_char_p]
x.XOpenDisplay.restype = c.c_void_p
x.XGetInputFocus.argtypes = [c.c_void_p, c.POINTER(c.c_ulong), c.POINTER(c.c_int)]
x.XFetchName.argtypes = [c.c_void_p, c.c_ulong, c.POINTER(c.c_void_p)]
x.XQueryTree.argtypes = [c.c_void_p, c.c_ulong, c.POINTER(c.c_ulong), c.POINTER(c.c_ulong), c.POINTER(c.c_void_p), c.POINTER(c.c_uint)]
x.XFree.argtypes = [c.c_void_p]
# Windows can disappear between observation calls during desktop startup.
error_handler = c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_void_p)(lambda *_: 0)
x.XSetErrorHandler(error_handler)
display = x.XOpenDisplay(None)
if not display:
    raise SystemExit('Cannot observe X input focus')
while True:
    window, revert = c.c_ulong(), c.c_int()
    x.XGetInputFocus(display, c.byref(window), c.byref(revert))
    focused, titles = False, []
    for _ in range(20):
        if window.value in (0, 1):
            break
        name = c.c_void_p()
        if x.XFetchName(display, window, c.byref(name)) and name.value:
            title = c.string_at(name).decode(errors='replace')
            x.XFree(name)
            titles.append(title)
            focused |= title == 'portable-input-test'
        root, parent, children, count = c.c_ulong(), c.c_ulong(), c.c_void_p(), c.c_uint()
        if not x.XQueryTree(display, window, c.byref(root), c.byref(parent), c.byref(children), c.byref(count)):
            break
        if children.value:
            x.XFree(children)
        if parent.value == window.value:
            break
        window = parent
    status = {'focused': focused, 'titles': titles, 'observedAt': time.time() * 1000}
    Path('/probe/focus.tmp').write_text(json.dumps(status))
    Path('/probe/focus.tmp').replace('/probe/focus.json')
    time.sleep(0.05)
