"""Validate deterministic CPU A/B logs and report the measured CPU reduction."""
import argparse
from pathlib import Path
import re
from statistics import median


def read_run(path):
    text = path.read_text()
    text = text[text.rfind('[BUILD]'):]
    mode = re.search(r'\[CPU\] event-driven controller=(\d)', text)
    if not mode or '[CPU_BENCH]' not in text:
        raise ValueError(f'{path}: not a CPU benchmark')
    checks = {int(f): state for f, state in re.findall(r'\[CPU_CHECK\] frame=(\d+) ([^\n]+)', text)}
    times = {int(f): float(t) for t, f in re.findall(
        r'\[TIMING\] ms/frame cpu=([0-9.]+).*?\[CPU_CHECK\] frame=(\d+)', text, re.S)}
    quads = {int(f): int(q) for f, q in re.findall(r'\[GXM_PREP\] frame=(\d+) [^\n]*qn=(\d+)', text)}
    if set(checks) != set(range(60, 1201, 60)):
        raise ValueError(f'{path}: incomplete 1200-frame benchmark')
    if re.search(r'\[RR\] frame \d+  traps [1-9]|fault 1|FATAL', text):
        raise ValueError(f'{path}: runtime fault')
    return int(mode[1]), checks, times, quads


def compare(old, new):
    am, ac, at, aq = read_run(old)
    bm, bc, bt, bq = read_run(new)
    if (am, bm) != (0, 1):
        raise ValueError('Expected legacy log then native log')
    for f in ac:
        if ac[f] != bc[f]:
            raise ValueError(f'State mismatch frame {f}:\n{ac[f]}\n{bc[f]}')
        if aq.get(f) != bq.get(f):
            raise ValueError(f'Rendered geometry count mismatch frame {f}')
    print(f'PASS: {len(ac)} checkpoints match all register/WRAM/polygon/shared CRCs and budgets')
    for label, frames in [('Steady state', [f for f in ac if f >= 180]),
                          ('Race geometry', [f for f in ac if f >= 180 and aq.get(f, 0) > 1000])]:
        if not frames:
            raise ValueError(f'No {label} samples')
        before, after = median(at[f] for f in frames), median(bt[f] for f in frames)
        reduction = 100 * (1 - after / before)
        print(f'{label}: {len(frames)} samples, median CPU {before:.3f} -> {after:.3f} ms/frame, {reduction:.1f}% reduction')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('legacy', type=Path); p.add_argument('native', type=Path)
    a = p.parse_args()
    compare(a.legacy, a.native)
