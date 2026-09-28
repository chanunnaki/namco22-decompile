"""Attach native CPU controller to the immutable upstream translation.

Refuse changed instruction blocks: the native contract must be reviewed when
the upstream lifter changes. Output is a build artifact, never a second source.
"""
import argparse
import hashlib
from pathlib import Path

def attach(source):
    source = '#include "rr_cpu.h"\n' + source
    hooks = {
        '40EE': 'if (rr_cpu_native) { uint32_t t_ = rr_cpu_frame_loop(); if (t_ == 0xffffffffu) return; pc_ = t_; goto resume_; }',
        '2B05C': 'if (rr_cpu_native) { rr_cpu_wait_service(); goto A_2B064; }',
    }
    for start, end, digest in CONTRACTS:
        block = source[source.index('A_' + start + ':'):source.index('A_' + end + ':')]
        if hashlib.sha256(block.encode()).hexdigest() != digest:
            raise ValueError('CPU contract changed at ' + start + '; revalidate native controller')
    for label, hook in hooks.items():
        needle = 'A_' + label + ': RR_INS('
        assert source.count(needle) == 1
        source = source.replace(needle, 'A_' + label + ': ' + hook + '\n  RR_INS(')
    return source

CONTRACTS = [('40EE', '40F6', '0538acb67036fec42dcdb70798db7b5cafe475dbbbe1a84acdd97554ab39f2c3'), ('40F6', '411E', 'e26d5a9335d0ec5ece206e24431e1f7c40bb4afd2c468bb2cbbe5dd3eb944689'), ('2B05C', '2B064', '83d9122b5a6fd0ca0917b99d04644fff2813d5dd728dd990ed1f999615dd31d2')]

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('source', type=Path); p.add_argument('output', type=Path)
    a = p.parse_args()
    a.output.write_text(attach(a.source.read_text()))
