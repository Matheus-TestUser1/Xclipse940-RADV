#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare named AMD register references with Samsung's MGFX2 kernel headers.

Source references are review candidates, not proof that a path executes on X940.
The JSON retains line numbers and excerpts so generation guards and packet
ranges can be reviewed. The tool never edits the source or submits GPU work.
"""

import argparse
import collections
import json
from pathlib import Path
import re


def numeric_defines(path):
    return {
        name: int(value, 0)
        for name, value in re.findall(
            r"^\s*#define\s+(\w+)\s+(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*\b", path.read_text(), re.M
        )
    }


def kernel_map(offsets, ip_offsets):
    definitions = numeric_defines(offsets)
    bases = numeric_defines(ip_offsets)
    result = {}
    for name, offset in definitions.items():
        if not name.startswith("mm") or name.endswith("_BASE_IDX"):
            continue
        index = definitions.get(name + "_BASE_IDX")
        base_name = f"GC_BASE__INST0_SEG{index}"
        if index is not None and base_name in bases:
            result[name[2:]] = 4 * (bases[base_name] + offset)
    if len(result) < 1000:
        raise ValueError("Incomplete GC header mapping; verify the offset and IP-base headers")
    return result


def strip_comments_and_strings(text):
    def blank(match):
        return "".join("\n" if char == "\n" else " " for char in match[0])
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"', blank, text, flags=re.S)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--offset-header", type=Path, required=True)
    parser.add_argument("--ip-header", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    mapping = kernel_map(args.offset_header, args.ip_header)
    aliases = collections.defaultdict(list)
    for name, addr in mapping.items():
        aliases[addr].append(name)

    # This gate validates the port's named addresses independently of the port
    # implementation, against the kernel headers supplied by the caller.
    port_header = root / "src/amd/common/ac_mgfx2_regs.h"
    enum_values = re.findall(r"AC_MGFX2_(\w+)\s*=\s*(0x[0-9a-fA-F]+)u", port_header.read_text())
    errors = []
    for name, value in enum_values:
        if mapping.get(name) != int(value, 16):
            errors.append({"name": name, "port": value, "kernel": mapping.get(name)})

    references = []
    files = sorted(p for p in (root / "src/amd").rglob("*") if p.suffix in {".c", ".cpp", ".h"})
    for path in files:
        original = path.read_text(errors="replace")
        clean = strip_comments_and_strings(original)
        original_lines = original.splitlines()
        for match in re.finditer(r"\bR_([0-9A-F]{6})_([A-Z0-9_]+)\b", clean):
            addr, name = int(match[1], 16), match[2]
            line = clean.count("\n", 0, match.start()) + 1
            kernel_addr = mapping.get(name)
            excerpt = "\n".join(original_lines[max(0, line - 2):line + 1])
            if kernel_addr == addr:
                status = "same_address"
            elif "ac_mgfx2_reg(" in excerpt or "ac_x940_reg_v25(" in excerpt:
                status = "explicit_mapping_nearby"
            elif kernel_addr is not None:
                status = "moved_name_candidate"
            else:
                status = "name_missing_candidate"
            references.append({
                "file": str(path.relative_to(root)), "line": line, "name": name,
                "amd_address": f"0x{addr:06x}",
                "mgfx2_address": f"0x{kernel_addr:06x}" if kernel_addr is not None else None,
                "status": status, "mgfx2_aliases_at_amd_address": aliases.get(addr, []),
                "excerpt": excerpt,
            })
    counts = dict(collections.Counter(row["status"] for row in references))
    result = {
        "scope": "AMD source references; execution guards require manual review",
        "files_scanned": len(files), "kernel_named_registers": len(mapping),
        "port_named_registers_checked": len(enum_values), "port_map_errors": errors,
        "counts": counts, "references": references,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items() if key != "references"}, indent=2))
    print(f"Report: {args.output}")
    raise SystemExit(1 if errors else 0)


if __name__ == "__main__":
    main()
