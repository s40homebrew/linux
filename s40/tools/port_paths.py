"""Locations shared by the port's optional host and firmware checks.

Ordinary builds need only this repository and the SDK. Firmware, IDA databases,
emulator state and captures belong to a separate research workspace.
"""
# SPDX-License-Identifier: MIT
import os
import shutil
from pathlib import Path


def configured(name, default):
    return Path(os.environ.get(name, str(default))).expanduser().resolve()


PROJECT = Path(__file__).resolve().parents[1]
KERNEL_ROOT = configured("KERNEL_SOURCE", PROJECT.parent)
SDK_ROOT = configured("S40_SDK_ROOT", PROJECT.parents[1] / "main")
RESEARCH_ROOT = configured("S40_RESEARCH_ROOT", PROJECT.parents[1])
LINUX_RESEARCH_ROOT = configured(
    "S40_LINUX_RESEARCH_ROOT", RESEARCH_ROOT / "analysis/linux_6303/research"
)
STATE_ROOT = configured(
    "S40_LINUX_STATE_ROOT", RESEARCH_ROOT / "analysis/linux_6303/emulator"
)
EMULATOR_ROOT = configured("S40_EMULATOR_ROOT", RESEARCH_ROOT.parent / "nokia_6303c_emu")
QEMU = configured("QEMU", EMULATOR_ROOT / "vendor/qemu/build/qemu-system-arm")


def llvm_tool(name):
    """Honor LLVM overrides, then PATH and the macOS keg-only tools."""
    if os.environ.get("LLVM"):
        candidate = Path(os.environ["LLVM"]) / name
        if candidate.is_file():
            return str(candidate)
    found = shutil.which(name)
    if found:
        return found
    candidate = Path("/opt/homebrew/opt/llvm/bin") / name
    return str(candidate) if candidate.is_file() else None
