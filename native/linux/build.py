#!/usr/bin/env python3
"""Build the deterministic Linux window bridge archive.

On Windows this script delegates to WSL so the checked-in archive is a real
Linux x86_64 object. Applications consume the archive; end users do not need
headers, a C compiler, or wayland-scanner.
"""

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys


HERE = pathlib.Path(__file__).resolve().parent
PACKAGE = HERE.parent.parent
OUTPUT = PACKAGE / "libdolet_window_linux.a"


def run(command: list[str], cwd: pathlib.Path | None = None) -> None:
    print("+", " ".join(command))
    subprocess.run(command, cwd=cwd, check=True)


def wsl_path(path: pathlib.Path) -> str:
    value = subprocess.check_output(["wsl.exe", "wslpath", "-a", str(path)], text=True)
    return value.strip()


def build_linux() -> None:
    build = HERE / ".build"
    if build.exists():
        shutil.rmtree(build)
    build.mkdir()

    protocol_root = pathlib.Path("/usr/share/wayland-protocols")
    protocols = {
        "xdg-shell": protocol_root / "stable/xdg-shell/xdg-shell.xml",
        "relative-pointer-unstable-v1": protocol_root / "unstable/relative-pointer/relative-pointer-unstable-v1.xml",
        "pointer-constraints-unstable-v1": protocol_root / "unstable/pointer-constraints/pointer-constraints-unstable-v1.xml",
    }
    for name, xml in protocols.items():
        if not xml.exists():
            raise SystemExit(f"missing Wayland protocol: {xml}")
        run(["wayland-scanner", "client-header", str(xml), str(build / f"{name}-client-protocol.h")])
        run(["wayland-scanner", "private-code", str(xml), str(build / f"{name}-protocol.c")])

    cflags = subprocess.check_output(
        ["pkg-config", "--cflags", "wayland-client", "wayland-cursor", "xkbcommon", "x11", "vulkan"],
        text=True,
    ).split()
    sources = [HERE / "dolet_window_linux.c"] + [build / f"{name}-protocol.c" for name in protocols]
    objects: list[pathlib.Path] = []
    for source in sources:
        obj = build / (source.stem + ".o")
        run([
            "cc", "-std=c11", "-O2", "-fPIC", "-fvisibility=hidden",
            "-ffile-prefix-map=" + str(HERE) + "=.",
            "-I", str(build), *cflags, "-c", str(source), "-o", str(obj),
        ])
        objects.append(obj)
    run(["ar", "rcD", str(OUTPUT), *map(str, objects)])
    run(["ranlib", "-D", str(OUTPUT)])
    print(f"built {OUTPUT} ({OUTPUT.stat().st_size} bytes)")


def main() -> None:
    if os.name == "nt":
        distro = os.environ.get("DOLET_WSL_DISTRO", "Ubuntu-24.04")
        linux_script = wsl_path(pathlib.Path(__file__).resolve())
        run(["wsl.exe", "-d", distro, "--", "python3", linux_script])
    else:
        build_linux()


if __name__ == "__main__":
    main()
