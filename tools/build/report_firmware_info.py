#!/usr/bin/env python3
"""Post-link memory report for the ailink BSP.

Reads MEMORY regions from the linker script and section sizes from the ELF,
then prints ROM/RAM utilization with severity-colored bars. Wired into
rtconfig.py POST_ACTION; standalone:

    python3 tools/build/report_firmware_info.py build/ailink.elf
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROM_REGION = 'ROM'
RAM_REGION = 'RAM'
BAR_WIDTH = 24
WARN_PCT = 80.0
CRIT_PCT = 90.0

_MEMORY_RE = re.compile(
    r'(?P<name>\w+)\s*\([^)]*\)\s*:\s*ORIGIN\s*=\s*[^,]+,\s*'
    r'LENGTH\s*=\s*(?P<num>\d+)\s*(?P<unit>[KkMm]?)'
)
_UNIT = {'': 1, 'k': 1024, 'm': 1024 * 1024}


def fail(msg):
    print(f'[error] {msg}', file=sys.stderr)
    sys.exit(1)


class Style:
    """ANSI styling; degrades to plain text on non-tty output or NO_COLOR."""

    def __init__(self):
        on = sys.stdout.isatty() and 'NO_COLOR' not in os.environ
        code = lambda c: f'\033[{c}m' if on else ''
        self.bold, self.dim, self.reset = code(1), code(2), code(0)
        self.red, self.yellow, self.green = code(31), code(33), code(32)
        self.blue, self.cyan = code(34), code(36)

    def severity(self, pct):
        if pct >= CRIT_PCT:
            return self.red
        if pct >= WARN_PCT:
            return self.yellow
        return self.green


def find_tool(name):
    exec_path = os.environ.get('RTT_EXEC_PATH')
    if exec_path and os.access(Path(exec_path) / name, os.X_OK):
        return str(Path(exec_path) / name)
    tool = shutil.which(name)
    if not tool:
        fail(f'{name} not found (set RTT_EXEC_PATH or add it to PATH)')
    return tool


def parse_memory_regions(ldscript):
    """Return {region: length_bytes} from the MEMORY block of a linker script."""
    regions = {
        m['name']: int(m['num']) * _UNIT[m['unit'].lower()]
        for m in _MEMORY_RE.finditer(ldscript.read_text())
    }
    for required in (ROM_REGION, RAM_REGION):
        if required not in regions:
            fail(f'region {required} not found in {ldscript}')
    return regions


def read_section_sizes(elf):
    """Return (text, data, bss) in bytes via `size --format=berkeley`."""
    tool = find_tool('arm-none-eabi-size')
    result = subprocess.run([tool, '--format=berkeley', str(elf)],
                            capture_output=True, text=True)
    if result.returncode != 0:
        fail(result.stderr.strip() or f'{tool} exited {result.returncode}')
    for line in result.stdout.splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 3 and fields[0].isdigit():
            return int(fields[0]), int(fields[1]), int(fields[2])
    fail(f'unexpected output from {tool}:\n{result.stdout}')


def human(n):
    if n < 1024:
        return f'{n}B'
    if n < 1024 ** 2:
        return f'{n / 1024:.1f}K'
    return f'{n / 1024 ** 2:.2f}M'


def usage_line(label, used, total, s):
    pct = used / total * 100
    filled = min(BAR_WIDTH, round(BAR_WIDTH * pct / 100))
    color = s.severity(pct)
    bar = f'{color}{"█" * filled}{s.reset}{s.dim}{"░" * (BAR_WIDTH - filled)}{s.reset}'
    return (f'  {s.bold}{label:<4}{s.reset} {bar} {color}{pct:5.1f}%{s.reset}'
            f' {human(used):>7} / {human(total)}')


def render_report(elf, mode, regions, text, data, bss, s):
    rom_used = text + data
    ram_used = data + bss
    rule = s.dim + '─' * 58 + s.reset
    mode_color = s.blue if mode == 'Debug' else s.green

    lines = [
        rule,
        f'  {s.bold}{s.cyan}{elf.stem}{s.reset} · {mode_color}{mode}{s.reset}',
        rule,
        usage_line(ROM_REGION, rom_used, regions[ROM_REGION], s),
        usage_line(RAM_REGION, ram_used, regions[RAM_REGION], s),
        f'  {s.dim}text {human(text)}  data {human(data)}  bss {human(bss)}'
        f'  elf {human(elf.stat().st_size)}',
    ]
    bin_file = elf.with_suffix('.bin')
    if bin_file.exists():
        lines[-1] += f'  bin {human(bin_file.stat().st_size)}'
    lines[-1] += s.reset

    worst = max(rom_used / regions[ROM_REGION], ram_used / regions[RAM_REGION]) * 100
    if worst >= WARN_PCT:
        color = s.severity(worst)
        level = 'critically high' if worst >= CRIT_PCT else 'high'
        lines.append(f'  {color}{s.bold}warning: memory usage is {level}{s.reset}')
    lines.append(rule)
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('elf', type=Path, help='linked ELF file')
    parser.add_argument('--ldscript', type=Path,
                        default=Path('board/linker_scripts/link.lds'),
                        help='linker script defining MEMORY regions')
    parser.add_argument('--mode', choices=('Debug', 'Release'),
                        default=os.environ.get('BUILD_MODE', 'Release'),
                        help='build mode label (default: $BUILD_MODE)')
    args = parser.parse_args()

    for path in (args.elf, args.ldscript):
        if not path.is_file():
            fail(f'file not found: {path}')

    regions = parse_memory_regions(args.ldscript)
    text, data, bss = read_section_sizes(args.elf)
    print(render_report(args.elf, args.mode, regions, text, data, bss, Style()))


if __name__ == '__main__':
    main()
