#!/usr/bin/env python3
"""Execute objdump's RV32 mtx4_mul listing and count retired instructions.

This small integer interpreter validates the compiler's RV32 loop/spill choices.
It measures instructions and memory operations, never hardware cycles or FPS.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import random
import re

MASK = (1 << 32)-1

def signed(x):
    return x if x < (1 << 31) else x-(1 << 32)

def read_program(path):
    program = {}
    for line in path.read_text().splitlines():
        match = re.match(r'^([0-9a-f]+):\s+([0-9a-f]+)\s+(\w+)\s*(.*)', line)
        if match:
            addr, code, op, rest = match.groups()
            operands = rest.split('#')[0].strip().split(',')
            program[int(addr,16)] = (op, operands, len(code)//2)
    assert program
    return program

def execute(program, a, b, alias):
    regs = Counter(a0=0x4000, a1=0x1000, a2=0x2000, sp=0x8000, gp=0x6000)
    if alias == 'a': regs['a0'] = regs['a1']
    if alias == 'b': regs['a0'] = regs['a2']
    if alias == 'both': regs['a0'] = regs['a2'] = regs['a1']; b = a
    memory = {0x1000+4*i: x for i,x in enumerate(a)}
    if alias != 'both': memory.update({0x2000+4*i: x for i,x in enumerate(b)})
    counts = Counter()
    pc = min(program)
    def address(s):
        m = re.fullmatch(r'(-?\d+)\((\w+)\)',s)
        return (int(m[1])+regs[m[2]])&MASK
    for _ in range(10000):
        op, p, size = program[pc]
        pc += size
        counts[op] += 1
        if op == 'ret': break
        if op == 'lw': regs[p[0]] = memory.get(address(p[1]),0)
        elif op == 'sw': memory[address(p[1])] = regs[p[0]]
        elif op == 'mv': regs[p[0]] = regs[p[1]]
        elif op == 'li': regs[p[0]] = int(p[1],0)
        elif op == 'lui': regs[p[0]] = int(p[1],0)<<12
        elif op == 'addi': regs[p[0]] = regs[p[1]]+int(p[2],0)
        elif op == 'add': regs[p[0]] = regs[p[1]]+regs[p[2]]
        elif op == 'or': regs[p[0]] = regs[p[1]]|regs[p[2]]
        elif op == 'sltu': regs[p[0]] = int(regs[p[1]]<regs[p[2]])
        elif op == 'slli': regs[p[0]] = regs[p[1]]<<int(p[2],0)
        elif op == 'srli': regs[p[0]] = regs[p[1]]>>int(p[2],0)
        elif op == 'mul': regs[p[0]] = regs[p[1]]*regs[p[2]]
        elif op == 'mulh': regs[p[0]] = (signed(regs[p[1]])*signed(regs[p[2]]))>>32
        elif op == 'bne':
            if regs[p[0]] != regs[p[1]]: pc=int(p[2].split()[0],16)
        else: raise AssertionError('Unsupported instruction: '+op)
        for r in regs: regs[r] &= MASK
        regs['zero'] = 0
    else: raise AssertionError('Function did not return')
    expected=[]
    for row in range(4):
        for col in range(3):
            dot=sum(signed(a[row*4+k])*signed(b[k*4+col]) for k in range(3))
            expected.append(((dot>>16)+(b[12+col] if row==3 else 0))&MASK)
        expected.append(65536 if row==3 else 0)
    actual=[memory[regs['a0']+4*i] for i in range(16)]
    assert actual==expected, (alias,actual,expected)
    return counts

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('listing',type=Path)
    args=parser.parse_args()
    program=read_program(args.listing)
    rng=random.Random(371)
    for _ in range(1000):
        a=[rng.getrandbits(32) for _ in range(16)]
        b=[rng.getrandbits(32) for _ in range(16)]
        for alias in ('none','a','b','both'):
            counts=execute(program,a,b,alias)
    print(json.dumps(dict(cases=4000, instructions=sum(counts.values()),
                         loads=counts['lw'], stores=counts['sw'], opcodes=dict(counts))))

if __name__=='__main__': main()
