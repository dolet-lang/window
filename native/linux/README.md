# Linux native window bridge

This target-owned bridge gives the public `window` package two runtime
backends behind one stable Dolet API:

1. Wayland + xdg-shell (preferred when `WAYLAND_DISPLAY` is usable).
2. X11/XWayland fallback.

It also owns native Vulkan surface creation, software framebuffer
presentation, XKB text input, relative-pointer locking, and pointer
constraints. Frog and Eqoi therefore never import a display-server ABI.

`DOLET_WINDOW_BACKEND=wayland` or `DOLET_WINDOW_BACKEND=x11` forces a backend
for testing. The default is `auto`.

Run `python native/linux/build.py` after changing the bridge. On Windows it
uses WSL and writes the deterministic `libdolet_window_linux.a` archive at the
package root.
