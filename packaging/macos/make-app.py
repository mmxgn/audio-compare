#!/usr/bin/env python3
"""Build a self-contained "Audio Compare.app" (and optionally a .dmg) on macOS.

Uses the Homebrew GTK 4 / libadwaita / GStreamer stack:

    brew install meson ninja pkg-config gtk4 libadwaita gstreamer libebur128 librsvg
    packaging/macos/make-app.py          # -> dist/Audio Compare.app
    packaging/macos/make-app.py --dmg    # -> also dist/audio-compare-<ver>-<arch>.dmg

Every non-system dylib the app, its GStreamer plugins and the SVG pixbuf loader
need is copied into Contents/Frameworks and relinked to @rpath, so the bundle
does not depend on /opt/homebrew at run time. The result is ad-hoc signed and
runs on the macOS version the Homebrew bottles target (see LSMinimumSystemVersion)
and newer, on the architecture it was built on.
"""

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
APP_NAME = "Audio Compare"

# Only what the app's pipelines use (uridecodebin -> audioconvert ->
# audioresample -> audioamplify/volume -> audiomixer -> autoaudiosink, plus an
# appsink for peaks), and decoders for the formats it advertises plus a few
# common extras (flac, opus, aiff, m4a/aac).
GST_PLUGINS = [
    "coreelements", "typefindfunctions", "playback", "app", "pbtypes",
    "audioconvert", "audioresample", "audiomixer", "audiofx", "volume",
    "autodetect", "osxaudio",
    "wavparse", "aiff", "ogg", "vorbis", "opus", "flac", "mpg123",
    "audioparsers", "id3demux", "apetag", "isomp4", "faad",
]

GTK_SCHEMAS = "org.gtk.gtk4.Settings.*.gschema.xml"

SYSTEM_PREFIXES = ("/usr/lib/", "/System/")


def run(*cmd, **kw):
    r = subprocess.run(cmd, text=True, capture_output=True, **kw)
    if r.returncode != 0:
        raise SystemExit(f"command failed ({r.returncode}): {' '.join(map(str, cmd))}\n{r.stderr}")
    return r.stdout


def is_macho(p: Path) -> bool:
    try:
        with open(p, "rb") as f:
            magic = f.read(4)
    except OSError:
        return False
    return magic in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")


def load_commands(p: Path):
    """Return (id, [deps], [rpaths], minos) parsed from `otool -l`."""
    ident, deps, rpaths, minos = None, [], [], None
    cmd = None
    for line in run("otool", "-l", str(p)).splitlines():
        line = line.strip()
        if line.startswith("cmd "):
            cmd = line.split()[1]
        elif line.startswith("name ") and cmd in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB"):
            deps.append(line[5:].rsplit(" (offset", 1)[0])
        elif line.startswith("name ") and cmd == "LC_ID_DYLIB":
            ident = line[5:].rsplit(" (offset", 1)[0]
        elif line.startswith("path ") and cmd == "LC_RPATH":
            rpaths.append(line[5:].rsplit(" (offset", 1)[0])
        elif line.startswith("minos ") and cmd == "LC_BUILD_VERSION":
            minos = line.split()[1]
    return ident, deps, rpaths, minos


class Bundler:
    def __init__(self, app: Path, brew: Path):
        self.app = app
        self.brew = brew
        self.fw = app / "Contents/Frameworks"
        self.fw.mkdir(parents=True, exist_ok=True)
        self.bundled = {}  # library key (basename of its install id) -> source path
        self.minos = "11.0"

    def resolve(self, dep: str, src: Path, src_rpaths):
        """Map a load-command path to a real file, or None for system libraries."""
        if dep.startswith(SYSTEM_PREFIXES):
            return None
        loader_dir = src.resolve().parent
        if dep.startswith("@loader_path/"):
            cand = [loader_dir / dep[len("@loader_path/"):]]
        elif dep.startswith("@rpath/"):
            tail = dep[len("@rpath/"):]
            cand = [Path(r.replace("@loader_path", str(loader_dir))) / tail for r in src_rpaths]
            cand.append(self.brew / "lib" / tail)
        elif dep.startswith("@executable_path/"):
            raise SystemExit(f"{src}: unsupported @executable_path dependency {dep}")
        else:
            cand = [Path(dep)]
        for c in cand:
            if c.exists():
                return c.resolve()
        raise SystemExit(f"{src}: cannot resolve dependency {dep}")

    def key_for(self, lib: Path) -> str:
        ident = load_commands(lib)[0]
        return os.path.basename(ident) if ident else lib.name

    def relink(self, dst: Path, src: Path, rpath: str, set_id: bool):
        """Copy-side fix-up: point deps at @rpath, replace rpaths, queue new deps."""
        ident, deps, rpaths, minos = load_commands(src)
        if minos and tuple(map(int, minos.split("."))) > tuple(map(int, self.minos.split("."))):
            self.minos = minos
        args = []
        if set_id:
            args += ["-id", f"@rpath/{dst.name}"]
        pending = []
        for dep in deps:
            real = self.resolve(dep, src, rpaths)
            if real is None:
                continue
            key = self.key_for(real)
            args += ["-change", dep, f"@rpath/{key}"]
            if key not in self.bundled:
                self.bundled[key] = real
                pending.append((key, real))
        for r in rpaths:
            args += ["-delete_rpath", r]
        args += ["-add_rpath", rpath]
        dst.chmod(0o755)
        run("install_name_tool", *args, str(dst))
        for key, real in pending:
            out = self.fw / key
            shutil.copy2(real, out)
            self.relink(out, real, "@loader_path", set_id=True)

    def add(self, src: Path, dst: Path, rpath: str):
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        self.relink(dst, src, rpath, set_id=False)


def brew_prefix(formula=None) -> Path:
    args = ["brew", "--prefix"] + ([formula] if formula else [])
    return Path(run(*args).strip())


def project_version() -> str:
    m = re.search(r"version\s*:\s*'([^']+)'", (ROOT / "meson.build").read_text())
    return m.group(1) if m else "0.0.0"


def build_binary(build_dir: Path) -> Path:
    if not (build_dir / "build.ninja").exists():
        subprocess.run(["meson", "setup", str(build_dir), "--buildtype=release"], cwd=ROOT, check=True)
    subprocess.run(["ninja", "-C", str(build_dir)], cwd=ROOT, check=True)
    return build_dir / "audio-compare"


def make_icns(res: Path):
    svg = ROOT / "data/icons/hicolor/scalable/apps/org.mmxgn.audiocompare.svg"
    with tempfile.TemporaryDirectory() as tmp:
        iconset = Path(tmp) / "audio-compare.iconset"
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = size * scale
                name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
                run("rsvg-convert", "-w", str(px), "-h", str(px), "-o", str(iconset / name), str(svg))
        run("iconutil", "-c", "icns", "-o", str(res / "audio-compare.icns"), str(iconset))


def add_resources(app: Path, brew: Path):
    res = app / "Contents/Resources"
    share = res / "share"
    res.mkdir(parents=True, exist_ok=True)

    make_icns(res)

    schemas = share / "glib-2.0/schemas"
    schemas.mkdir(parents=True)
    for xml in (brew / "share/glib-2.0/schemas").glob(GTK_SCHEMAS):
        shutil.copy2(xml, schemas)
    run("glib-compile-schemas", str(schemas))

    icons = share / "icons/hicolor"
    (icons / "scalable/apps").mkdir(parents=True)
    shutil.copy2(brew / "share/icons/hicolor/index.theme", icons)
    shutil.copy2(ROOT / "data/icons/hicolor/scalable/apps/org.mmxgn.audiocompare.svg", icons / "scalable/apps")

    (res / "etc/fonts").mkdir(parents=True)
    shutil.copy2(HERE / "fonts.conf", res / "etc/fonts/fonts.conf")


def add_pixbuf_svg_loader(bundler: Bundler, app: Path):
    # The SVG loader lets the About dialog show the full-colour app icon.
    rsvg = brew_prefix("librsvg")
    loader = rsvg / "lib/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader_svg.so"
    dst = app / "Contents/Resources/lib/gdk-pixbuf" / loader.name
    bundler.add(loader, dst, "@loader_path/../../../Frameworks")

    cache = run("gdk-pixbuf-query-loaders", str(loader))
    cache = cache.replace(str(loader), f"@LIB@/gdk-pixbuf/{loader.name}")
    cache = "\n".join(l for l in cache.splitlines() if not l.startswith("#")) + "\n"
    (app / "Contents/Resources/gdk-pixbuf-loaders.cache.in").write_text(cache)


def verify(app: Path):
    bad = []
    for p in app.rglob("*"):
        if p.is_file() and not p.is_symlink() and is_macho(p):
            _, deps, rpaths, _ = load_commands(p)
            for d in deps + rpaths:
                if d.startswith(("/", )) and not d.startswith(SYSTEM_PREFIXES):
                    bad.append(f"{p.relative_to(app)}: {d}")
    if bad:
        raise SystemExit("bundle still references outside libraries:\n  " + "\n  ".join(bad))


def sign(app: Path):
    # Sign nested code first, then the bundle (ad-hoc: no Developer ID).
    machos = [p for p in app.rglob("*") if p.is_file() and not p.is_symlink() and is_macho(p)]
    for p in machos:
        run("codesign", "--force", "--sign", "-", "--timestamp=none", str(p))
    run("codesign", "--force", "--sign", "-", "--timestamp=none", str(app))
    run("codesign", "--verify", "--deep", "--strict", str(app))


def make_dmg(app: Path, out: Path, version: str) -> Path:
    dmg = out / f"audio-compare-{version}-{platform.machine()}.dmg"
    dmg.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / APP_NAME
        stage.mkdir()
        shutil.copytree(app, stage / app.name, symlinks=True)
        os.symlink("/Applications", stage / "Applications")
        run("hdiutil", "create", "-volname", APP_NAME, "-srcfolder", str(stage),
            "-ov", "-format", "UDZO", str(dmg))
    return dmg


def main():
    if sys.platform != "darwin":
        raise SystemExit("make-app.py only runs on macOS")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dmg", action="store_true", help="also wrap the app in a .dmg")
    ap.add_argument("--out", type=Path, default=ROOT / "dist", help="output folder (default: dist/)")
    ap.add_argument("--build-dir", type=Path, default=ROOT / "build-macos-app")
    args = ap.parse_args()

    brew = brew_prefix()
    gst = brew_prefix("gstreamer")
    version = project_version()

    binary = build_binary(args.build_dir)

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    app = out / f"{APP_NAME}.app"
    if app.exists():
        shutil.rmtree(app)
    macos = app / "Contents/MacOS"
    macos.mkdir(parents=True)

    bundler = Bundler(app, brew)
    bundler.add(binary, macos / "audio-compare-bin", "@executable_path/../Frameworks")
    bundler.add(gst / "libexec/gstreamer-1.0/gst-plugin-scanner", macos / "gst-plugin-scanner",
                "@executable_path/../Frameworks")
    for name in GST_PLUGINS:
        lib = brew / f"lib/gstreamer-1.0/libgst{name}.dylib"
        if not lib.exists():
            raise SystemExit(f"missing GStreamer plugin {lib}")
        bundler.add(lib, app / f"Contents/Resources/lib/gstreamer-1.0/{lib.name}", "@loader_path/../../../Frameworks")
    (app / "Contents/Resources/gio-modules").mkdir(parents=True)  # empty: keep Homebrew GIO modules out

    add_resources(app, brew)
    add_pixbuf_svg_loader(bundler, app)

    shutil.copy2(HERE / "launcher.sh", macos / "audio-compare")
    (macos / "audio-compare").chmod(0o755)
    plist = (HERE / "Info.plist.in").read_text()
    plist = plist.replace("@VERSION@", version).replace("@MINOS@", bundler.minos)
    (app / "Contents/Info.plist").write_text(plist)

    verify(app)
    sign(app)
    print(f"{app}  ({len(bundler.bundled)} libraries, macOS {bundler.minos}+)")

    if args.dmg:
        print(make_dmg(app, out, version))


if __name__ == "__main__":
    main()
