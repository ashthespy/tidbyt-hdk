#!/usr/bin/env python3
import os
import sys
import subprocess
import argparse


def parse_addresses(backtrace_line):
    """
    Given a line like
      "Backtrace: 0x400d49a7:0x3ffd17d0 0x400d40c0:0x3ffd17f0 …"
    return ["0x400d49a7", "0x400d40c0", …]
    """
    # strip leading "Backtrace:" (case-insensitive)
    parts = backtrace_line.strip().split()
    if parts and parts[0].lower().startswith("backtrace"):
        parts = parts[1:]
    addrs = []
    for p in parts:
        addr = p.split(":", 1)[0]
        if addr.startswith("0x"):
            addrs.append(addr)
    return addrs


def collapse_path(path, home, cwd):
    norm = path.replace("\\", "/")
    if norm.startswith(cwd + "/"):
        return norm.replace(cwd + "/", "./")
    if norm.startswith(home + "/"):
        return norm.replace(home + "/", "~/")
    return norm


def main():
    home = os.path.expanduser("~").replace("\\", "/")
    cwd = os.getcwd().replace("\\", "/")
    p = argparse.ArgumentParser(
        description="Decode an ESP backtrace via xtensa-esp32-elf-addr2line"
    )
    p.add_argument(
        "backtrace",
        help="The full backtrace line, e.g. "
        "'Backtrace: 0x400d49a7:0x3ffd17d0 0x400d40c0:…'",
    )
    p.add_argument(
        "--toolchain-path",
        default=os.path.join(home, ".platformio/packages/toolchain-xtensa-esp32/bin"),
        help="Path to the xtensa-esp32-elf-* toolchain bin folder",
    )
    p.add_argument(
        "--project",
        default="TIXEL",
        help="PlatformIO project name (used to locate ELF at .pio/build/<project>/firmware.elf)",
    )
    args = p.parse_args()

    # locate addr2line
    exe = "xtensa-esp32-elf-addr2line"
    if os.name == "nt":
        exe += ".exe"
    addr2line = os.path.join(args.toolchain_path, exe)
    if not os.path.isfile(addr2line):
        sys.exit(f"ERROR: addr2line not found at {addr2line}")

    # locate ELF via project name
    elf = os.path.join(".pio", "build", args.project, "firmware.elf")
    if not os.path.isfile(elf):
        sys.exit(
            f"ERROR: ELF file not found at {elf} (did you pass the correct --project?)"
        )

    # parse addresses
    addrs = parse_addresses(args.backtrace)
    if not addrs:
        sys.exit("ERROR: No valid addresses found in backtrace.")

    # run addr2line for each
    for addr in addrs:
        print(f"\n[{addr}]")
        result = subprocess.run(
            [addr2line, "-pfiaC", "-e", elf, addr],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        if result.returncode != 0 or not result.stdout:
            pass
        output = result.stdout.strip()
        if " at " in output:
            func, loc = output.split(" at ", 1)
            print(f"{func} at {collapse_path(loc, home, cwd)}")
        else:
            print(output)


if __name__ == "__main__":
    main()
