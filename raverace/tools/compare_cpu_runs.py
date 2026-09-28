"""Validate deterministic A/B logs and report CPU or DSP time reduction."""
import argparse
from pathlib import Path
import re
from statistics import median


def read_run(path, component="cpu"):
    text = path.read_text()
    text = text[text.rfind('[BUILD]'):]
    mode = re.search(r'\[CPU\] event-driven controller=(\d)' if component == 'cpu' else r'\[DSP_NATIVE\] enabled=(\d)', text)
    if not mode or '[CPU_BENCH]' not in text:
        raise ValueError(f'{path}: not a CPU benchmark')
    checks = {int(f): state for f, state in re.findall(r'\[CPU_CHECK\] frame=(\d+) ([^\n]+)', text)}
    times = {int(f): float(t) for t, f in re.findall(
        r'\[TIMING\] ms/frame [^\n]*?'+component+r'=([0-9.]+).*?\[CPU_CHECK\] frame=(\d+)', text, re.S)}
    quads = {int(f): int(q) for f, q in re.findall(r'\[GXM_PREP\] frame=(\d+) [^\n]*qn=(\d+)', text)}
    stop = re.search(r'\[CPU_BENCH\][^\n]*stop=(\d+)', text)
    end = int(stop[1]) if stop else 1200
    if set(checks) != set(range(60, end + 1, 60)):
        raise ValueError(f'{path}: incomplete scripted benchmark')
    if re.search(r'\[RR\] frame \d+  traps [1-9]|fault 1|FATAL', text):
        raise ValueError(f'{path}: runtime fault')
    return int(mode[1]), checks, times, quads


def compare(old, new, component="cpu"):
    am, ac, at, aq = read_run(old, component)
    bm, bc, bt, bq = read_run(new, component)
    if (am, bm) != (0, 1):
        raise ValueError('Expected legacy log then native log')
    if ac.keys() != bc.keys():
        raise ValueError('Benchmark frame ranges differ')
    old_text, new_text = old.read_text(), new.read_text()
    old_spec = re.findall(r'\[CPU_BENCH\] ([^\n]+)', old_text)[-1]
    new_spec = re.findall(r'\[CPU_BENCH\] ([^\n]+)', new_text)[-1]
    if old_spec != new_spec:
        raise ValueError('Benchmark input sequences differ')
    race_start = re.search(r'race-from=(\d+)', old_spec)
    race_from = int(race_start[1]) if race_start else 180
    race_frames = [f for f in ac if f >= race_from and aq.get(f, 0) > (0 if race_start else 1000)]
    if race_start:
        # Reject selection/countdown data from the driving benchmark.
        pattern = r'\[BENCH_RACE\] frame=(\d+) mode=(\d+) menu=\d+ speed=(\d+)'
        states = [{int(f): (int(m), int(s)) for f, m, s in re.findall(pattern, text)}
                  for text in (old_text, new_text)]
        if states[0] != states[1]:
            raise ValueError('Race mode/speed diagnostics differ')
        if any(states[0].get(f, (0, 0))[0] != 3 or states[0][f][1] == 0
               for f in race_frames):
            raise ValueError('Driving benchmark contains a stationary/non-race checkpoint')
    for f in ac:
        if ac[f] != bc[f]:
            raise ValueError(f'State mismatch frame {f}:\n{ac[f]}\n{bc[f]}')
        if aq.get(f) != bq.get(f):
            raise ValueError(f'Rendered geometry count mismatch frame {f}')
    print(f'PASS: {len(ac)} checkpoints match all register/WRAM/polygon/shared CRCs and budgets')
    for label, frames in [('Steady state', [f for f in ac if f >= 180]),
                          ('Moving race' if race_start else 'Race geometry', race_frames)]:
        if not frames:
            raise ValueError(f'No {label} samples')
        before, after = median(at[f] for f in frames), median(bt[f] for f in frames)
        reduction = 100 * (1 - after / before)
        print(f'{label}: {len(frames)} samples, median {component.upper()} {before:.3f} -> {after:.3f} ms/frame, {reduction:.1f}% reduction')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--component', choices=['cpu', 'dsp'], default='cpu')
    p.add_argument('legacy', type=Path); p.add_argument('native', type=Path)
    a = p.parse_args()
    compare(a.legacy, a.native, a.component)
