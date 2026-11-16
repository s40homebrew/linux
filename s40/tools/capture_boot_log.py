#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Capture the boot trace and Linux log from the existing QMP session."""
import argparse
import itertools
import json
import re
import sys
from pathlib import Path

from boot_trace import decode_traces, relocation_checks
from emulator_session import EMU, STATE
from read_ramlog import decode

sys.path.insert(0, str(EMU / "scripts"))
from capture_nor import QMP


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new evidence directory")
    args = parser.parse_args()
    directory = args.output.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    state = json.loads(STATE.read_text())
    qmp = QMP(state["qmp"])
    try:
        running = qmp.command("query-status")["running"]
        qmp.command("stop")
        try:
            watchdogs = {name: qmp.command("qom-get", path="/machine", property=name)
                         for name in ("pmic-watchdog-reloads", "soc-watchdog-reloads",
                                      "pmic-watchdog-expiries", "soc-watchdog-expiries")}
            qmp.command("pmemsave", val=0x10000000, size=0x2000000, filename=str(directory / "ram.bin"))
            registers = qmp.command("human-monitor-command", **{"command-line": "info registers"})
            (directory / "registers.txt").write_text(registers)
            qmp.command("screendump", filename=str(directory / "screen.png"), format="png")
        finally:
            if running:
                qmp.command("cont")
    finally:
        qmp.close()
    memory = (directory / "ram.bin").read_bytes()
    pc_match = re.search(r"R15=([0-9a-fA-F]+)", registers)
    cpsr_match = re.search(r"PSR=([0-9a-fA-F]+)", registers)
    pc = int(pc_match[1], 16) if pc_match else -1
    cpsr = int(cpsr_match[1], 16) if cpsr_match else -1
    plans = []
    for plan in decode_traces(memory):
        if plan["stage"] >= 2:
            offset = plan["log_pa"] - 0x10000000
            if 0 <= offset <= len(memory) - 4096:
                page = memory[offset:offset + 4096]
                (directory / "ramlog.bin").write_bytes(page)
                try:
                    log = decode(page)
                    (directory / "linux.log").write_bytes(log)
                    plan["linux_log"] = log.decode(errors="replace")
                    # The 4 KiB ring wraps during a continuing boot; recent
                    # PID1 records are stronger evidence than an old banner.
                    heartbeats = re.findall(rb"HEARTBEAT (\d+) uptime=(\d+)", log)
                    advancing = any(int(b[0]) > int(a[0]) and int(b[1]) > int(a[1])
                                    for a, b in itertools.pairwise(heartbeats))
                    irq_counts = [int(n) for n in re.findall(
                        rb"\d+:\s+(\d+)\s+RAPS\s+36\s+Level\s+raps-timer2", log)]
                    pings = [int(n) for n in re.findall(rb"WATCHDOG pings=(\d+)", log)]
                    plan["cpu_pc"] = pc
                    plan["cpu_cpsr"] = cpsr
                    plan["watchdogs"] = watchdogs
                    plan["checks"] = {
                        "userspace_pid1": b"USERSPACE READY pid=1" in log or bool(heartbeats),
                        "process_memory_selftest": b"SELFTEST PASS: fork mmap waitpid" in log,
                        "scheduler_clock_advance": advancing,
                        "timer_irqs_advance": len(irq_counts) >= 2 and irq_counts[-1] > irq_counts[0] > 0,
                        "keypad_status_menu": b"key code=2\n" in log and b"/proc/meminfo" in log,
                        "keypad_process_test": b"key code=4\n" in log and b"SELFTEST PASS: fork mmap waitpid" in log,
                        "watchdog_userspace_feeding": len(pings) >= 2 and pings[-1] > pings[0] > 0,
                        "both_watchdogs_serviced": all(watchdogs[name] > 1 for name in watchdogs if "reloads" in name),
                        "no_watchdog_expiry": all(watchdogs[name] == 0 for name in watchdogs if "expiries" in name),
                        "no_failure_in_captured_log": not any(marker in log for marker in
                            (b"Kernel panic", b"Internal error:", b"SELFTEST FAIL", b"Out of memory:",
                             b"WATCHDOG keepalive FAIL", b"WATCHDOG unavailable", b"handover failed")),
                    }
                    if plan['format'] == 2:
                        image_bytes = (Path(__file__).resolve().parents[1] / 'kernel/build/Image').stat().st_size
                        plan['checks'].update(relocation_checks(plan, image_bytes))
                    plan["passed"] = all(plan["checks"].values())
                except ValueError as error:
                    plan["log_error"] = str(error)
        plans.append(plan)
    (directory / "boot-plans.json").write_text(json.dumps(plans, indent=2) + "\n")
    print(json.dumps(plans, indent=2))
    if not plans:
        print("No boot trace found; inspect registers and screenshot.", file=sys.stderr)
        return 1
    return 0 if any(plan.get("passed") for plan in plans) else 2


if __name__ == "__main__":
    raise SystemExit(main())
