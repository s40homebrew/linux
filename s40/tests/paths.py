# SPDX-License-Identifier: MIT
"""Locate public source and ARM tools for standalone host checks."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from port_paths import KERNEL_ROOT, llvm_tool  # noqa: E402,F401
