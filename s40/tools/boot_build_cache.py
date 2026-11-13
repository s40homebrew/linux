#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Cache Linux launcher builds."""
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
SDK_ROOT = Path(os.environ.get("S40_SDK_ROOT", str(PROJECT.parents[1] / "main"))).resolve()
SHARED = SDK_ROOT / "homebrew"
APP = PROJECT / "boot_probe"
BUILD = PROJECT / "build/boot_probe"
DIST = PROJECT / "dist"
STATE = BUILD / "build-inputs.json"
ARTIFACTS = (DIST / "Nokia6303Linux.jar", DIST / "Nokia6303Linux.jad")


def artifact_state(path):
    stat = path.stat()
    return {"size": stat.st_size, "mtime_ns": stat.st_mtime_ns}


def files_under(path):
    return sorted(item for item in path.rglob("*") if item.is_file()
                  and "__pycache__" not in item.parts)


def fingerprint(files, metadata=()):
    digest = hashlib.sha256(json.dumps(metadata, sort_keys=True).encode())
    for path in sorted(set(files)):
        digest.update(str(path).encode() + b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def tool_path(value):
    found = shutil.which(value)
    if found is None:
        raise FileNotFoundError(f"missing build tool: {value}")
    return Path(found).resolve()


def inputs():
    env = os.environ
    # build_boot_probe.sh sources the public SDK's tool discovery first.
    llvm = env["LLVM"]
    java = Path(env["JAVA8_HOME"])
    compiler = tool_path(env.get("ARM_GCC", "arm-none-eabi-gcc"))
    tools = [tool_path(env.get("CC", str(Path(llvm) / "clang"))),
             compiler, tool_path(env.get("LD", "ld.lld")),
             *(tool_path(str(Path(llvm) / name)) for name in
               ("llvm-objcopy", "llvm-objdump", "llvm-nm", "llvm-readobj")),
             tool_path(str(java / "bin/java")), tool_path(str(java / "bin/javap")),
             Path(sys.executable).resolve(), tool_path("bash")]
    tool_stats = [(str(path), path.stat().st_size, path.stat().st_mtime_ns) for path in tools]
    # Record resolved tool identities above, not the raw PATH. pyenv changes
    # PATH when a build is launched from Python even if every tool is identical;
    # hashing that spelling needlessly invalidates the complete native build.
    inherited = {name: env.get(name, "") for name in
                 ("LLVM", "CC", "ARM_GCC", "LD", "JAVA8_HOME", "CPATH",
                  "C_INCLUDE_PATH", "LIBRARY_PATH", "GCC_EXEC_PREFIX", "JAVA_TOOL_OPTIONS",
                  "JDK_JAVA_OPTIONS", "SOURCE_DATE_EPOCH")}
    sources = [PROJECT / "build_boot_probe.sh", PROJECT / "build_handoff.sh",
               PROJECT / "tools/boot_build_cache.py", PROJECT / "tools/embed_payload.py",
               PROJECT / "tools/handoff_layout.py", PROJECT / "kernel/build/Image",
               PROJECT / "kernel/build/image-layout.json", SHARED / "build.sh",
               SDK_ROOT / "tools/env.sh", SDK_ROOT / "scripts/clean_build_outputs.py",
               PROJECT / "kernel/overlay/arch/arm/mach-nokia-raps/rm443_boot_marker.h",
               PROJECT / "kernel/overlay/arch/arm/mach-nokia-raps/rm443_secure_wdt_abi.h",
               APP / "build-config.sh"]
    for path in (APP / "native", APP / "src", PROJECT / "handoff",
                 SHARED / "runtime/include",
                 SHARED / "runtime/libc/include", SHARED / "bootstrap"):
        sources.extend(files_under(path))
    for directory in ('runtime/native', 'runtime/java'):
        sources.extend(path for path in (SHARED / directory).iterdir() if path.is_file())
    for directory in ("resolver", "profiles"):
        sources.extend((SHARED / directory).glob("*.java"))
    # Match the shared build's actual tools. Editing an unrelated maintenance
    # packager must not recompile Linux; packager changes only need repackaging.
    sources.extend(SHARED / 'tools' / name for name in
                   ('gen_imports.py', 'pack.py', 'gen_native_limits.py', 'preverify.py'))
    for key, default in (
        ("PROGUARD_JAR", SDK_ROOT / "tools/.deps/proguard-7.10.0/lib/proguard.jar"),
        ("LEGACY_JAVAC", SDK_ROOT / "tools/.deps/legacy-javac.jar"),
        ("SDK_CLASSES", SDK_ROOT / "tools/.deps/S40-SDK-Classes/S40v6/classes/classes.zip"),
    ):
        sources.append(Path(env.get(key, str(default))).resolve())
    for name in ("libc.a", "libm.a", "libgcc.a"):
        path = subprocess.check_output([str(compiler), "-print-file-name=" + name], text=True).strip()
        sources.append(Path(path).resolve())
    package = [APP / "MANIFEST.MF", SHARED / "COPYING", SHARED / "tools/package_midlet.py"]
    for path in (APP / "licenses", SHARED / "licenses"):
        if path.is_dir():
            package.extend(files_under(path))
    return {"build": fingerprint(sources, (tool_stats, inherited)),
            "package": fingerprint(package)}


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("check", "store"):
        raise SystemExit("usage: boot_build_cache.py check|store")
    current = inputs()
    if sys.argv[1] == "store":
        current["artifacts"] = {path.name: artifact_state(path) for path in ARTIFACTS}
        current["payload"] = fingerprint(files_under(BUILD / "preverified"))
        BUILD.mkdir(parents=True, exist_ok=True)
        temporary = STATE.with_suffix(".new")
        temporary.write_text(json.dumps(current, indent=2) + "\n")
        temporary.replace(STATE)
        return
    try:
        previous = json.loads(STATE.read_text())
    except (OSError, ValueError):
        print("full")
        return
    if previous.get("build") != current["build"]:
        print("full")
    elif previous.get("package") != current["package"]:
        # Repackage only if the preverified payload still exists. A removed
        # build directory must never silently produce an incomplete archive.
        required = ("application.bin", "native.bin", "stage.bin", "NativeSession.class")
        payload_valid = all((BUILD / "preverified" / name).is_file() for name in required)
        payload_valid = payload_valid and previous.get("payload") == fingerprint(files_under(BUILD / "preverified"))
        print("package" if payload_valid else "full")
    elif all(path.is_file() and previous.get("artifacts", {}).get(path.name) == artifact_state(path)
             for path in ARTIFACTS):
        print("cached")
    else:
        print("full")


if __name__ == "__main__":
    main()
