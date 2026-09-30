"""Static check of a generated tree: every function-table entry whose address is not the start of the function it points to
must be a `case` of that function's resume switch (otherwise a resume at that address would restart the function from
its first instruction). Also reports table entries pointing at unknown functions and, for functions generated in dsp
mode (ps2_cg_dsp.h), that every `goto label_X` has its label.
Usage: verify_resume_table.py <generated dir>"""
import os, re, sys

gen = sys.argv[1]
table_file = None
for name in os.listdir(gen):
    if name.endswith('.cpp') and 'function_table' in name.lower():
        table_file = name
if table_file is None:
    for name in os.listdir(gen):
        if name.endswith('.cpp') and not name.startswith(('FUN_', 'entry_', 'caseD_')):
            with open(os.path.join(gen, name), errors='replace') as f:
                if 'g_ps2RecompiledFunctionTable[' in f.read(20000):
                    table_file = name
                    break
print('table file:', table_file)
entries = re.findall(r'g_ps2RecompiledFunctionTable\[\d+\] = (\w+); // 0x([0-9a-f]+)',
                     open(os.path.join(gen, table_file), errors='replace').read())
print('table entries:', len(entries))

cases_cache = {}
start_cache = {}


def info(fn):
    if fn in cases_cache:
        return start_cache[fn], cases_cache[fn]
    path = os.path.join(gen, fn + '.cpp')
    if not os.path.exists(path):
        cases_cache[fn] = None
        start_cache[fn] = None
        return None, None
    text = open(path, errors='replace').read()
    m = re.search(r'// Address: 0x([0-9a-f]+) - 0x([0-9a-f]+)', text)
    start = int(m.group(1), 16) if m else None
    cases = {int(c, 16) for c in re.findall(r'case 0x([0-9A-Fa-f]+)u: goto label_', text)}
    labels = set(re.findall(r'^label_([0-9a-f]+):', text, re.M))
    gotos = set(re.findall(r'goto label_([0-9a-f]+);', text))
    missing = gotos - labels
    if missing:
        print('MISSING LABELS in', fn, sorted(missing)[:5])
    cases_cache[fn] = cases
    start_cache[fn] = start
    return start, cases


bad = 0
unknown = 0
mapped_inside = 0
for fn, addr_hex in entries:
    addr = int(addr_hex, 16)
    start, cases = info(fn)
    if cases is None:
        # stubs / library functions are declared in a stubs file, not one file per function
        unknown += 1
        continue
    if start is None or addr == start:
        continue
    mapped_inside += 1
    if addr not in cases:
        bad += 1
        if bad <= 20:
            print(f'NOT A RESUME CASE: table 0x{addr:x} -> {fn} (function starts 0x{start:x})')
print(f'entries mapped to a function containing the address: {mapped_inside}; without a resume case: {bad}; '
      f'no function file (stubs): {unknown}')
sys.exit(1 if bad else 0)
