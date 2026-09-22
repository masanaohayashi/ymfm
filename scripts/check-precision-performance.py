#!/usr/bin/env python3
"""Compare the same render workload against the pre-optimization Release core.
Manual performance regression gate; do not run concurrently with other benchmarks.
"""
import argparse
import pathlib
import subprocess
import tempfile
import sys

root = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('reference', nargs='?', default='00e09ac')
parser.add_argument('--repeats', type=int, default=3)
parser.add_argument('--case', action='append', help='Case to evaluate; repeat to select several')
parser.add_argument('--min-speedup', type=float, help='Override the original regression thresholds')
args = parser.parse_args()
if args.repeats < 1:
    parser.error('--repeats must be positive')
reference = args.reference
with tempfile.TemporaryDirectory(prefix='ymfm-perf-') as temporary:
    directory=pathlib.Path(temporary)
    (directory/'ymfm_precision.h').write_bytes(subprocess.check_output(
        ['git','show',reference+':src/ymfm_precision.h'],cwd=root))
    outputs={}
    for name,include in [('baseline',directory),('current',root/'src')]:
        binary=directory/name
        subprocess.run(['c++','-std=c++14','-O3','-DNDEBUG','-I'+str(include),
            str(root/'benchmarks/precision.cpp'),'-o',str(binary)],check=True)
        outputs[name]={}
        for line in subprocess.check_output([str(binary),str(args.repeats)],text=True).splitlines():
            case,elapsed,checksum=line.split()
            outputs[name][case]=float(elapsed)
    if args.case:
        unknown = set(args.case) - outputs['baseline'].keys()
        if unknown:
            parser.error('unknown cases: '+', '.join(sorted(unknown)))
    failed=False
    for case,before in outputs['baseline'].items():
        if args.case and case not in args.case:continue
        after=outputs['current'][case]
        ratio=before/after
        threshold=args.min_speedup if args.min_speedup is not None else (5.0 if case.endswith(('idle','released')) else 2.0)
        ok=ratio>=threshold
        print(f'{case:14} {before:.6f}s -> {after:.6f}s  {ratio:.2f}x  '
              f'{"PASS" if ok else "FAIL"} (>= {threshold:.1f}x)',flush=True)
        failed|=not ok
    sys.exit(1 if failed else 0)
