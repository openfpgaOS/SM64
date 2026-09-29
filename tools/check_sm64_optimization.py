#!/usr/bin/env python3
"""Build ROM-free regression tests; optionally compare a saved source baseline."""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def build(out, source, name, sanitize=False):
    flags = ['-std=gnu99', '-O3', '-ffast-math', '-fno-strict-aliasing', '-fwrapv',
             '-ffunction-sections', '-fdata-sections', '-DOF_PC', '-DTARGET_OPENFPGA',
             '-DSM64_PROFILE=0', '-DENABLE_SOFTRAST', '-DF3DEX_GBI_2E',
             '-DNO_SEGMENTED_MEMORY', '-DAVOID_UB', '-DVERSION_US', '-D_LANGUAGE_C', '-DNON_MATCHING']
    if sanitize:
        flags += ['-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    includes = [ROOT/'src/sdk/include', source/'sm64/include', source/'sm64/src',
                source/'sm64/src/pc', source/'sm64', source]
    for p in includes:
        flags += ['-I', str(p)]
    inputs = [ROOT/'tools/tests'/f'sm64_{name}.c']
    if name == 'renderer':
        inputs += [source/'sm64/src/pc/gfx/gfx_cc.c', source/'sm64/src/pc/configfile.c']
    if name == 'matrix':
        inputs += [source/'pocket/fx32_mtx.c']
    subprocess.run([os.environ.get('CC', 'cc'), *flags, *map(str, inputs),
                    '-Wl,--gc-sections', '-lm', '-o', str(out)], check=True)

def run(exe, *args):
    return subprocess.check_output([str(exe), *args], text=True).strip()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-src', type=Path, help='saved src/sm64 directory')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    results = {}
    with tempfile.TemporaryDirectory(prefix='sm64-tests-') as tmp:
        tmp = Path(tmp)
        for name in ('renderer', 'matrix', 'audio_clock', 'voices', 'draw_order'):
            exe = tmp/name
            build(exe, ROOT/'src/sm64', name, args.sanitize)
            results[name] = run(exe)
            print(results[name], flush=True)
            if name == 'renderer':
                results['renderer_regressions'] = run(exe, 'regressions')
                print(results['renderer_regressions'], flush=True)
                results['renderer_rectangles'] = run(exe, 'rectangles')
                print(results['renderer_rectangles'], flush=True)
                results['renderer_decals'] = run(exe, 'decals')
                print(results['renderer_decals'], flush=True)
                results['renderer_menus'] = run(exe, 'menus')
                print(results['renderer_menus'], flush=True)
                results['palettized_trace'] = run(exe, 'palettized')
                print(results['palettized_trace'], flush=True)
            if args.baseline_src and name not in ('audio_clock', 'draw_order'):
                baseline = tmp/(name+'-baseline')
                build(baseline, args.baseline_src.resolve(), name)
                if name == 'renderer':
                    results['baseline_trace'] = run(baseline)
                    assert results['baseline_trace'] == results[name], results
                    assert run(baseline, 'palettized') == results['palettized_trace']
                if name == 'voices':
                    previous = run(baseline)
                    assert previous.split()[:2] == results[name].split()[:2]
                    results['baseline_voices'] = previous
                if not args.sanitize and name != 'voices':
                    samples = {'before': [], 'after': []}
                    for i in range(9):
                        order = [('before', baseline), ('after', exe)]
                        if i%2: order.reverse()
                        for label, binary in order:
                            samples[label].append(float(run(binary, 'bench').split()[1]))
                    before, after = (statistics.median(samples[k]) for k in ('before','after'))
                    results[name+'_host_benchmark'] = dict(samples_ns=samples,
                        before_median_ns=before, after_median_ns=after,
                        reduction_percent=100*(1-after/before))
                    print(json.dumps(results[name+'_host_benchmark']), flush=True)
    if args.output:
        args.output.write_text(json.dumps(results, indent=2)+'\n')

if __name__ == '__main__':
    main()
