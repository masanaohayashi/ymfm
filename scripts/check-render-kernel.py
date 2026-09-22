#!/usr/bin/env python3
"""Reject libm calls and division instructions reachable from render()."""
import platform
import pathlib
import re
import subprocess
import sys
import tempfile
root=pathlib.Path(__file__).resolve().parents[1]
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
clang='clang' in subprocess.check_output([compiler,'--version'],text=True).lower()
with tempfile.TemporaryDirectory(prefix='ymfm-render-audit-') as directory:
    for variant,defines in [('native',[]),('scalar',['-DYMFM_PRECISION_DISABLE_SIMD'])]:
        asm=pathlib.Path(directory)/'kernel.s'
        obj=pathlib.Path(directory)/'kernel.o'
        common=[compiler,'-std=c++14','-O3','-DNDEBUG','-I'+str(root/'src'),str(root/'tests/render_kernel.cpp')]
        subprocess.run(common+defines+['-S','-o',str(asm)],check=True)
        subprocess.run(common+defines+['-c','-o',str(obj)],check=True)
        undefined=subprocess.check_output(['nm','-u',str(obj)],text=True)
        allowed={'memset','memcpy','memmove','bzero','stack_chk_fail','stack_chk_guard','GLOBAL_OFFSET_TABLE_'}
        bad_symbols=[]
        for line in undefined.splitlines():
            if not line.strip():continue
            name=line.split()[-1].lstrip('_')
            if name not in allowed:bad_symbols.append(line)
        divide=re.compile(r'^\s*(?:fdiv|[su]div|v?div(?:ss|sd|ps|pd)|idiv[qlwb]?|div[qlwb]?)\b',re.M)
        bad_instructions=divide.findall(asm.read_text())
        if bad_symbols or bad_instructions:
            print('FAIL: render contains external calls or division:',bad_symbols,bad_instructions)
            sys.exit(1)
        if variant == 'native' and clang and platform.machine() in ('arm64','aarch64'):
            assembly=asm.read_text()
            for lanes in ('4s','2d'):
                if not re.search(r'fmla(?:\.'+lanes+r'\b|\s+v\d+\.'+lanes+r'\b)',assembly):
                    raise SystemExit('FAIL: missing AArch64 packet FMA '+lanes)
        print('PASS:',variant,'float32/float64 render has no libm calls or division instructions')
        print('External symbols:',undefined.strip() or '(none)')
