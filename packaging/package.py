#!/usr/bin/env python3
"""Build portable distributions using only Python's standard library and SDK tools."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile


def run(*args: str | Path, env=None) -> str:
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, env=env)
    if result.returncode:
        raise RuntimeError(f"{Path(str(args[0])).name} exited {result.returncode}: {result.stderr.strip() or result.stdout.strip()}")
    return result.stdout


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def copy_tree(source: Path, destination: Path) -> None:
    if source.is_dir():
        shutil.copytree(source, destination, dirs_exist_ok=True, ignore=shutil.ignore_patterns(".DS_Store", "*.pyc"))


def resources(args, root: Path, windows: bool) -> list[Path]:
    data = root / "share/advisual"
    data.mkdir(parents=True, exist_ok=True)
    for directory in ("icons", "themes"):
        copy_tree(args.source / directory, data / directory)
    for filename in ("advisual.ini", "docs.txt"):
        shutil.copy2(args.source / filename, data / filename)
    shutil.copy2(args.source / "packaging/dependency-sources.json", data / "dependency-sources.json")
    shutil.copy2(args.source / "packaging/LICENSES-SOURCES.txt", root / "LICENSES-SOURCES.txt")
    if (args.source / ".git").exists():
        commit = run("git", "-C", args.source, "rev-parse", "HEAD").strip()
        dirty = "yes" if run("git", "-C", args.source, "status", "--porcelain", "--untracked-files=no").strip() else "no"
    else:
        stamp = args.source / "SOURCE-COMMIT.txt"
        commit = stamp.read_text(encoding="utf-8").splitlines()[0] if stamp.is_file() else "unavailable (source archive)"
        dirty = "unavailable (source archive)"
    (root / "SOURCE-COMMIT.txt").write_text(commit + "\nRepository: https://github.com/othercat/AdVisual\nTracked working tree changes: " +
        dirty + "\n", encoding="ascii")
    licenses = root / "share/licenses/AdVisual"
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.source / "LICENSE.txt", licenses / "LICENSE.txt")
    for svg in (data / "icons").glob("*.svg"):
        run(args.prefix / "bin/rsvg-convert" if not windows else args.prefix / "bin/rsvg-convert.exe",
            "--width", "128", "--height", "128", "--output", svg.with_suffix(".png"), svg)
    schemas = root / "share/glib-2.0/schemas"
    schemas.mkdir(parents=True, exist_ok=True)
    for schema in (args.prefix / "share/glib-2.0/schemas").glob("*.gschema.xml"):
        shutil.copy2(schema, schemas / schema.name)
    if not list(schemas.glob("*.gschema.xml")):
        raise RuntimeError("GTK schema XML files are missing from the runtime prefix")
    run(args.prefix / ("bin/glib-compile-schemas.exe" if windows else "bin/glib-compile-schemas"), "--strict", schemas)
    copy_tree(args.prefix / "share/mime", root / "share/mime")
    for theme in ("Adwaita", "hicolor"):
        copy_tree(args.prefix / "share/icons" / theme, root / "share/icons" / theme)
    (root / "lib/gio/modules").mkdir(parents=True, exist_ok=True)
    fonts = root / "etc/fonts"
    fonts.mkdir(parents=True, exist_ok=True)
    font_dirs = "<dir>@SYSTEM_FONTS@</dir>" if windows else "<dir>/System/Library/Fonts</dir><dir>/Library/Fonts</dir><dir>~/Library/Fonts</dir>"
    (fonts / "fonts.conf.in").write_text(
        '<?xml version="1.0"?>\n<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">\n'
        '<fontconfig>' + font_dirs + '<cachedir prefix="xdg">fontconfig</cachedir>'
        '<alias><family>sans-serif</family><prefer><family>' + ("Segoe UI" if windows else "Helvetica") +
        '</family></prefer></alias><alias><family>monospace</family><prefer><family>' + ("Consolas" if windows else "Menlo") +
        '</family></prefer></alias></fontconfig>\n', encoding="utf-8")
    loader_dir = args.prefix / "lib/gdk-pixbuf-2.0/2.10.0/loaders"
    candidates = sorted(loader_dir.glob("*svg*.dll" if windows else "*svg*.dylib"))
    if not candidates and not windows:
        candidates = sorted(loader_dir.glob("*svg*.so"))
    if not candidates:
        raise RuntimeError("librsvg's GdkPixbuf SVG loader is missing")
    loader = candidates[0]
    destination = root / "lib/gdk-pixbuf-2.0/2.10.0/loaders" / loader.name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(loader, destination)
    query = args.prefix / ("bin/gdk-pixbuf-query-loaders.exe" if windows else "bin/gdk-pixbuf-query-loaders")
    lines = run(query, loader).splitlines()
    output = ["# Expanded into the current user's cache at startup; no build-host paths."]
    replaced = False
    for line in lines:
        if line.startswith("#"):
            continue
        if re.fullmatch(r'"(?:[^"\\]|\\.)+"', line) and "pixbufloader" in line:
            line = f'"@ADVISUAL_PREFIX@/lib/gdk-pixbuf-2.0/2.10.0/loaders/{loader.name}"'
            replaced = True
        output.append(line)
    if not replaced:
        raise RuntimeError("Cannot identify the SVG loader path in query-loaders output")
    (destination.parent.parent / "loaders.cache.in").write_text("\n".join(output) + "\n", encoding="utf-8")
    write_json(data / "portable-runtime.json", {"application": "AdVisual", "version": args.version, "format": 1})
    return [loader]


def source_licenses(args, root: Path) -> list[dict]:
    records = []
    lock = json.loads((args.source / "packaging/dependency-sources.json").read_text())
    if args.dependency_source:
        for name, metadata in lock.items():
            directory = args.dependency_source / name
            if not directory.is_dir():
                continue
            destination = root / "share/licenses" / name
            destination.mkdir(parents=True, exist_ok=True)
            for pattern in ("COPYING*", "LICENSE*", "LICENCE*", "NOTICE*"):
                for path in directory.glob(pattern):
                    if path.is_file():
                        shutil.copy2(path, destination / path.name)
            records.append({"name": name, **metadata})
    return records


def homebrew_licenses(libraries: list[Path], root: Path) -> list[dict]:
    formulas: dict[str, Path] = {}
    for library in libraries:
        parts = library.resolve().parts
        if "Cellar" in parts:
            index = parts.index("Cellar")
            formulas[parts[index + 1]] = Path(*parts[:index + 3])
    if not formulas:
        raise RuntimeError("Cannot identify the Homebrew runtime library provenance")
    metadata = json.loads(run("brew", "info", "--json=v2", *sorted(formulas)))["formulae"]
    records = []
    for formula in metadata:
        name = formula["name"]
        cellar = formulas[name]
        destination = root / "share/licenses" / name
        destination.mkdir(parents=True, exist_ok=True)
        for pattern in ("COPYING*", "LICENSE*", "LICENCE*", "NOTICE*", "AUTHORS*"):
            for path in cellar.glob(pattern):
                if path.is_file():
                    shutil.copy2(path, destination / path.name)
        copy_tree(cellar / "share/licenses", destination / "upstream")
        records.append({"name": name, "version": cellar.name, "license": formula.get("license"),
            "homepage": formula.get("homepage"), "source": formula.get("urls", {}).get("stable", {}).get("url"),
            "recipe": "https://github.com/Homebrew/homebrew-core"})
    return records


def windows_licenses(libraries: list[Path], root: Path) -> list[dict]:
    packages = set()
    for library in libraries:
        try:
            posix = run("cygpath", "-u", library).strip()
            packages.add(run("pacman", "-Qoq", posix).strip())
        except (RuntimeError, subprocess.CalledProcessError):
            continue  # AdPlug and libbinio are source-built, with their own recorded licenses.
    records = []
    if not packages:
        raise RuntimeError("No MSYS2 runtime license provenance was found")
    for package in sorted(packages):
        fields = {}
        for line in run("pacman", "-Qi", package).splitlines():
            match = re.match(r"^(Name|Version|URL|Licenses)\s+:\s+(.*)$", line)
            if match:
                fields[match[1].lower()] = match[2]
        for line in run("pacman", "-Qlq", package).splitlines():
            if "/share/licenses/" not in line or line.endswith("/"):
                continue
            source = Path(run("cygpath", "-w", line).strip())
            if source.is_file():
                relative = line.split("/share/licenses/", 1)[1]
                destination = root / "share/licenses" / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)
        records.append({**fields, "source_recipes": "https://github.com/msys2/MINGW-packages", "source_archives": "https://repo.msys2.org/mingw/sources/"})
    return records


def macho_dependencies(path: Path) -> list[str]:
    return [line.strip().split(" (compatibility", 1)[0] for line in run("/usr/bin/otool", "-L", path).splitlines()[1:] if line.strip()]


def macho_id(path: Path) -> str | None:
    lines = run("/usr/bin/otool", "-D", path).splitlines()
    return lines[1].strip() if len(lines) > 1 else None


def macho_rpaths(path: Path) -> list[str]:
    return re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.+?) \(offset", run("/usr/bin/otool", "-l", path))


def system_library(name: str) -> bool:
    return name.startswith(("/usr/lib/", "/System/Library/", "/Library/Apple/System/Library/"))


class MachOBundle:
    def __init__(self, frameworks: Path, prefix: Path):
        self.frameworks, self.prefix = frameworks, prefix
        self.files: dict[Path, Path] = {}
        self.edges: dict[Path, list[tuple[str, Path]]] = {}
        frameworks.mkdir(parents=True, exist_ok=True)

    def resolve(self, name: str, owner: Path) -> Path:
        candidates = []
        if name.startswith("@loader_path/"):
            candidates.append(owner.parent / name.removeprefix("@loader_path/"))
        elif name.startswith("@rpath/"):
            suffix = name.removeprefix("@rpath/")
            for rpath in macho_rpaths(owner):
                rpath = rpath.replace("@loader_path", str(owner.parent))
                if not rpath.startswith("@"):
                    candidates.append(Path(rpath) / suffix)
            candidates += [self.prefix / "lib" / suffix, owner.parent / suffix]
        else:
            candidates.append(Path(name))
        for path in candidates:
            if path.is_file():
                return path.resolve()
        raise RuntimeError(f"Unresolved Mach-O runtime dependency: {name} (from {owner.name})")

    def collect(self, source: Path, destination: Path | None = None) -> Path:
        source = source.resolve()
        if source in self.files:
            return self.files[source]
        destination = destination or self.frameworks / source.name
        if destination.exists() and destination not in self.files.values() and destination.resolve() != source:
            if hashlib.sha256(destination.read_bytes()).digest() != hashlib.sha256(source.read_bytes()).digest():
                raise RuntimeError(f"Conflicting runtime library basename: {destination.name}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            destination.chmod(destination.stat().st_mode | 0o200)
        shutil.copy2(source, destination)
        destination.chmod(destination.stat().st_mode | 0o200)
        self.files[source] = destination
        self.edges[source] = []
        identity = macho_id(source)
        for dependency in macho_dependencies(source):
            if dependency == identity or system_library(dependency):
                continue
            target = self.resolve(dependency, source)
            self.collect(target)
            self.edges[source].append((dependency, target))
        return destination

    def relocate(self) -> None:
        for source, destination in self.files.items():
            for previous, target in self.edges[source]:
                run("/usr/bin/install_name_tool", "-change", previous, "@rpath/" + self.files[target].name, destination)
            if macho_id(source):
                run("/usr/bin/install_name_tool", "-id", "@rpath/" + destination.name, destination)
            for rpath in macho_rpaths(destination):
                run("/usr/bin/install_name_tool", "-delete_rpath", rpath, destination)
            relative = os.path.relpath(self.frameworks, destination.parent)
            rpath = "@loader_path" if relative == "." else "@loader_path/" + relative
            run("/usr/bin/install_name_tool", "-add_rpath", rpath, destination)
            architectures = run("/usr/bin/lipo", "-archs", destination).split()
            if architectures != ["arm64"]:
                raise RuntimeError(f"Expected arm64 runtime library, got {architectures}: {destination.name}")
            build = run("/usr/bin/otool", "-l", destination)
            # LC_BUILD_VERSION is authoritative for current Homebrew dylibs.
            minimums = re.findall(r"minos (\d+\.\d+(?:\.\d+)?)", build)
            if any(tuple(map(int, version.split("."))) > (15, 0, 0) for version in minimums):
                raise RuntimeError(f"Dependency requires macOS newer than 15.0: {destination.name}")
            run("/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", destination)


def make_icns(args, resources_dir: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="advisual-icons-") as temporary:
        iconset = Path(temporary) / "AdVisual.iconset"
        iconset.mkdir()
        large = Path(temporary) / "large.png"
        run(args.prefix / "bin/rsvg-convert", "--width", "1024", "--height", "1024", "--output", large, args.source / "icons/advisual.svg")
        for size in (16, 32, 128, 256, 512):
            for factor in (1, 2):
                name = f"icon_{size}x{size}" + ("@2x" if factor == 2 else "") + ".png"
                run("/usr/bin/sips", "-z", str(size * factor), str(size * factor), large, "--out", iconset / name)
        run("/usr/bin/iconutil", "--convert", "icns", "--output", resources_dir / "AdVisual.icns", iconset)


def archive(args, package: Path, platform: str) -> None:
    name = f"AdVisual-v{args.version}-{platform}.zip"
    destination = args.output / name
    if platform == "macos-arm64":
        run("/usr/bin/ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", package, destination)
    else:
        with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as output:
            for path in sorted(package.rglob("*")):
                if path.is_file():
                    output.write(path, path.relative_to(package.parent).as_posix())
    digest = hashlib.sha256(destination.read_bytes()).hexdigest()
    (args.output / (name + ".sha256")).write_text(f"{digest}  {name}\n", encoding="ascii")
    print(destination)


def macos(args) -> None:
    app = args.output / "AdVisual.app"
    if app.exists():
        shutil.rmtree(app)
    contents = app / "Contents"
    runtime = contents / "Resources"
    loader_sources = resources(args, runtime, False)
    info = {"CFBundleExecutable": "advisual", "CFBundleIdentifier": "org.othercat.AdVisual", "CFBundleName": "AdVisual",
        "CFBundleDisplayName": "AdVisual", "CFBundlePackageType": "APPL", "CFBundleShortVersionString": args.version,
        "CFBundleVersion": args.version, "CFBundleIconFile": "AdVisual.icns", "LSMinimumSystemVersion": "15.0",
        "NSHighResolutionCapable": True, "LSApplicationCategoryType": "public.app-category.music"}
    (contents / "Info.plist").write_bytes(plistlib.dumps(info))
    make_icns(args, runtime)
    bundle = MachOBundle(contents / "Frameworks", args.prefix)
    bundle.collect(args.binary, contents / "MacOS/advisual")
    bundle.collect(args.probe, contents / "MacOS/advisual-check")
    for source in loader_sources:
        bundle.collect(source, runtime / "lib/gdk-pixbuf-2.0/2.10.0/loaders" / source.name)
    notices = homebrew_licenses(list(bundle.files), runtime) + source_licenses(args, runtime)
    write_json(runtime / "THIRD-PARTY.json", notices)
    write_json(runtime / "share/advisual/runtime-libraries.json", sorted(path.name for path in bundle.files.values()))
    bundle.relocate()
    run("/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", app)
    run("/usr/bin/codesign", "--verify", "--deep", "--strict", app)
    audit_macos(app)
    archive(args, app, "macos-arm64")


def pe_dependencies(path: Path) -> list[str]:
    return re.findall(r"DLL Name:\s*(\S+)", run("objdump", "-p", path), re.IGNORECASE)


def windows(args) -> None:
    package = args.output / "AdVisual"
    if package.exists():
        shutil.rmtree(package)
    loaders = resources(args, package, True)
    search = [args.prefix / "bin"] + args.dll_directory
    available = {path.name.lower(): path for directory in search for path in directory.glob("*.dll")}
    system = Path(os.environ["SystemRoot"]) / "System32"
    # PE imports can name system modules such as WINSPOOL.DRV as well as DLLs.
    system_names = {path.name.lower() for path in system.iterdir() if path.is_file()}
    sources: dict[Path, Path] = {}

    def collect(source: Path, destination: Path | None = None) -> None:
        source = source.resolve()
        if source in sources:
            return
        destination = destination or package / source.name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        sources[source] = destination
        for dependency in pe_dependencies(source):
            key = dependency.lower()
            if key in available:
                collect(available[key])
            elif key not in system_names and not key.startswith(("api-ms-win-", "ext-ms-win-")):
                raise RuntimeError(f"Missing portable DLL: {dependency}, required by {source.name}")

    collect(args.binary, package / "advisual.exe")
    collect(args.probe, package / "advisual-check.exe")
    for loader in loaders:
        collect(loader, package / "lib/gdk-pixbuf-2.0/2.10.0/loaders" / loader.name)
    write_json(package / "THIRD-PARTY.json", windows_licenses(list(sources), package) + source_licenses(args, package))
    write_json(package / "share/advisual/runtime-libraries.json", sorted(path.name for path in sources.values()))
    archive(args, package, "windows-x64")


def audit_macos(app: Path) -> None:
    frameworks = app / "Contents/Frameworks"
    binaries = list(frameworks.glob("*.dylib")) + list((app / "Contents/MacOS").iterdir())
    binaries += list((app / "Contents/Resources/lib/gdk-pixbuf-2.0/2.10.0/loaders").iterdir())
    for binary in binaries:
        identity = macho_id(binary)
        for dependency in macho_dependencies(binary):
            if dependency == identity or system_library(dependency):
                continue
            if not dependency.startswith("@rpath/") or not (frameworks / dependency.removeprefix("@rpath/")).is_file():
                raise RuntimeError(f"Bundle has an external dependency: {binary.name}: {dependency}")
        if any(not rpath.startswith("@loader_path") for rpath in macho_rpaths(binary)):
            raise RuntimeError(f"Bundle has a nonportable rpath: {binary.name}")


def check(args) -> None:
    package = args.package.resolve()
    with tempfile.TemporaryDirectory(prefix="AdVisual relocated ") as temporary:
        directory = Path(temporary)
        moved = directory / package.name
        shutil.copytree(package, moved)
        mac = package.suffix == ".app"
        if mac:
            audit_macos(moved)
            run("/usr/bin/codesign", "--verify", "--deep", "--strict", moved)
        env = {key: value for key, value in os.environ.items() if not key.startswith(("DYLD_", "GTK_", "GDK_", "GIO_", "GI_", "FONTCONFIG_", "XDG_"))}
        env["XDG_CONFIG_HOME"] = str(directory / "settings")
        env["XDG_CACHE_HOME"] = str(directory / "cache")
        env["GSETTINGS_BACKEND"] = "memory"
        env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin" if mac else str(Path(env["SystemRoot"]) / "System32")
        if mac:
            env["DYLD_PRINT_LIBRARIES"] = "1"
        else:
            env["APPDATA"] = str(directory / "settings")
        probe = moved / ("Contents/MacOS/advisual-check" if mac else "advisual-check.exe")
        result = subprocess.run([str(probe)], capture_output=True, text=True, env=env, timeout=30)
        if result.returncode:
            raise RuntimeError(f"Relocated GTK runtime check failed:\n{result.stdout}\n{result.stderr}")
        if mac and any(prefix in result.stderr for prefix in ("/opt/homebrew/", "/usr/local/Cellar/")):
            raise RuntimeError("Relocated process loaded an external Homebrew library")
        print(result.stdout.strip())
        executable = moved / ("Contents/MacOS/advisual" if mac else "advisual.exe")
        result = subprocess.run([str(executable), "--help"], capture_output=True, text=True, env=env, timeout=30)
        if result.returncode:
            raise RuntimeError(f"Relocated application startup failed:\n{result.stdout}\n{result.stderr}")
        if mac and any(prefix in result.stderr for prefix in ("/opt/homebrew/", "/usr/local/Cellar/")):
            raise RuntimeError("Relocated application startup loaded an external Homebrew library")
        print("PASS relocated application startup and default settings/theme initialization")
        print("PASS relocation: separate directory with spaces; clean runtime environment and PATH")


def main() -> None:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("macos", "windows"):
        sub = commands.add_parser(name)
        for argument in ("binary", "probe", "source", "output", "prefix"):
            sub.add_argument("--" + argument, type=Path, required=True)
        sub.add_argument("--version", required=True)
        sub.add_argument("--dependency-source", type=Path)
        sub.add_argument("--dll-directory", type=Path, action="append", default=[])
    checker = commands.add_parser("check")
    checker.add_argument("--package", type=Path, required=True)
    args = parser.parse_args()
    if args.command != "check":
        args.output.mkdir(parents=True, exist_ok=True)
        for argument in ("binary", "probe", "source", "output", "prefix"):
            setattr(args, argument, getattr(args, argument).resolve())
    {"macos": macos, "windows": windows, "check": check}[args.command](args)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
