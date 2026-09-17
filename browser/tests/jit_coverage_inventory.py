#!/usr/bin/env python3
"""Conservative source-only A32 -> IR -> WASM dispatch inventory (not a C++ parser)."""

import argparse
from collections import defaultdict, deque
import hashlib
import json
from pathlib import Path
import re
import sys

DYNARMIC = 'external/dynarmic/src/dynarmic'
TRANSLATE = DYNARMIC + '/frontend/A32/translate'
EMITTERS = [DYNARMIC + '/frontend/A32/a32_ir_emitter.cpp',
            DYNARMIC + '/ir/ir_emitter.cpp']
OPCODES = DYNARMIC + '/ir/opcodes.inc'
WASM = 'vita3k/cpu/src/wasmjit/emit_wasm.cpp'


def mask_cpp(text):
    """Blank comments/literals while preserving offsets and newlines."""
    pattern = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    return re.sub(pattern, lambda m: re.sub(r'[^\n]', ' ', m.group()), text)


def parse_opcodes(text):
    """Expand OPCODE/A32OPC/A64OPC macro names, preserving source locations."""
    result = {}
    for m in re.finditer(r'^\s*(OPCODE|A32OPC|A64OPC)\s*\(\s*(\w+)\s*,([^)]*)\)',
                         mask_cpp(text), re.M):
        macro, name, types = m.groups()
        name = {'OPCODE': '', 'A32OPC': 'A32', 'A64OPC': 'A64'}[macro] + name
        if name in result:
            raise ValueError('duplicate opcode: ' + name)
        result[name] = {'types': [t.strip() for t in types.split(',') if t.strip()],
                        'line': text.count('\n', 0, m.start(1)) + 1}
    if not result:
        raise ValueError('no OPCODE/A32OPC/A64OPC definitions found')
    return result


def balanced_end(text, start, opening='{', closing='}'):
    depth = 0
    for i in range(start, len(text)):
        if text[i] == opening:
            depth += 1
        elif text[i] == closing:
            depth -= 1
            if depth == 0:
                return i + 1
    raise ValueError('unbalanced ' + opening + ' at offset ' + str(start))


def functions(text, qualified=False):
    """Extract ordinary function bodies, including lambdas within their braces.

    Deliberately not a C++ grammar: no preprocessor expansion, overload resolution,
    constructor initializers, raw strings or operator-name parsing. Qualified mode
    recognizes the out-of-line IREmitter methods used by this checkout.
    """
    clean = mask_cpp(text)
    prefix = r'\bIREmitter::' if qualified else r'\b'
    pattern = prefix + r'(\w+)\s*\('
    result = []
    cursor = 0
    for match in re.finditer(pattern, clean):
        if match.start() < cursor:
            continue
        paren = clean.index('(', match.start())
        end = balanced_end(clean, paren, '(', ')')
        suffix = re.match(r'\s*(?:const\s*)?(?:noexcept\s*)?\{', clean[end:])
        if not suffix or match[1] in {'if', 'for', 'while', 'switch', 'catch'}:
            continue
        start = end + suffix.end() - 1
        cursor = balanced_end(clean, start)
        result.append({'name': match[1], 'start': start, 'end': cursor,
                       'line': text.count('\n', 0, match.start()) + 1,
                       'body': clean[start:cursor]})
    return result


def method_refs(clean):
    # Include member-function pointers (&IREmitter::Foo), used by A32 helpers.
    pattern = r'\bir\s*\.\s*(\w+)\s*(?:<[^;{}]*?>\s*)?\(|\bIREmitter::(\w+)'
    return [(m[1] or m[2], m.start()) for m in re.finditer(pattern, clean)]


def opcode_refs(clean):
    return [(m[1], m.start()) for m in re.finditer(r'\bOpcode::(\w+)', clean)]


def location(path, text, offset):
    return f'{path}:{text.count(chr(10), 0, offset) + 1}'


def reachability(sources, translators, definitions):
    """Method-name closure: union overloads/branches, with one witness per op."""
    methods = defaultdict(list)
    for path, text in sources.items():
        for function in functions(text, qualified=True):
            methods[function['name']].append((path, text, function))
    if not methods:
        raise ValueError('no IREmitter definitions found')
    roots = {}
    evidence = {}
    unresolved = defaultdict(list)
    for path, text in translators.items():
        clean = mask_cpp(text)
        for name, offset in method_refs(clean):
            witness = location(path, text, offset)
            if name in methods:
                roots.setdefault(name, [witness + ' -> ' + name])
            else:
                unresolved[name].append(witness)
        for name, offset in opcode_refs(clean):
            if name in definitions:
                evidence.setdefault(name, [location(path, text, offset) + ' -> ' + name])
    queue = deque(sorted(roots))
    paths = dict(roots)
    while queue:
        name = queue.popleft()
        for path, text, fn in methods[name]:
            witness = paths[name] + [f'{path}:{fn["line"]} -> IREmitter::{name}']
            for op, offset in opcode_refs(fn['body']):
                if op not in definitions:
                    raise ValueError('unknown emitted opcode: ' + op)
                evidence.setdefault(op, witness + [location(path, text, fn['start'] + offset) + ' -> ' + op])
            # Over-approximate unqualified calls and explicit method references;
            # names collide across overloads/classes by design, not type analysis.
            calls = set(re.findall(r'\b(\w+)\s*(?:<[^;{}]*?>\s*)?\(', fn['body']))
            calls.update(n for n, _ in method_refs(fn['body']))
            for callee in sorted(calls & methods.keys()):
                if callee not in paths:
                    paths[callee] = witness
                    queue.append(callee)
    return evidence, sorted(paths), {k: sorted(set(v)) for k, v in sorted(unresolved.items())}


def dispatch_inventory(text, definitions):
    """Discover dispatch cases only in instruction(), plus positive predicate routes.

    Other switches (e.g. flag-consumer scans) and comparisons in handler bodies
    are NOT dispatch evidence. Reachable helper references are kept separately.
    """
    funcs = {f['name']: f for f in functions(text)}
    if 'instruction' not in funcs:
        raise ValueError('missing WASM instruction() dispatcher')
    fn = funcs['instruction']
    body = fn['body']
    switch = re.search(r'\bswitch\s*\(\s*kind\s*\)\s*\{', body)
    if not switch:
        raise ValueError('unrecognized instruction() dispatch switch')
    switch_start = switch.end() - 1
    switch_end = balanced_end(body, switch_start)
    # Only outer switch labels. Nested switches must not inflate dispatch routes.
    switch_body = body[switch_start:switch_end]
    depth = 0
    depths = []
    for char in switch_body:
        depths.append(depth)
        depth += (char == '{') - (char == '}')
    routes = {}
    for m in re.finditer(r'\bcase\s+Op::(\w+)\s*:', switch_body):
        if depths[m.start()] == 1:
            routes[m[1]] = {'route': 'instruction switch case',
                            'source': location(WASM, text, fn['start'] + switch_start + m.start())}
    # The current dispatcher delegates Add/Sub and shifts before its switch.
    # Parse the positive equality predicates, rather than a hardcoded op list.
    for m in re.finditer(r'\bif\s*\(\s*(\w+)\(kind\)\s*\)\s*\{', body[:switch_start]):
        predicate = funcs.get(m[1])
        if not predicate:
            raise ValueError('unresolved dispatch predicate: ' + m[1])
        expr = re.fullmatch(r'\{\s*return\s+(.+?);\s*\}', predicate['body'], re.S)
        if not expr or not re.fullmatch(r'\s*op\s*==\s*Op::\w+(?:\s*\|\|\s*op\s*==\s*Op::\w+)*\s*', expr[1]):
            raise ValueError('unrecognized dispatch predicate: ' + m[1])
        for op in re.findall(r'Op::(\w+)', expr[1]):
            routes[op] = {'route': 'positive predicate ' + m[1],
                          'source': f'{WASM}:{predicate["line"]}'}
    unknown = set(routes) - definitions.keys()
    if unknown:
        raise ValueError('unknown dispatched opcodes: ' + ', '.join(sorted(unknown)))
    # Report rejection/exit sites in instruction and its lexically called helpers.
    # This is review evidence, not symbolic execution or per-op proof.
    pending = ['instruction']
    seen = set()
    guards = []
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        seen.add(name)
        helper = funcs[name]
        calls = set(re.findall(r'\b(\w+)\s*\(', helper['body']))
        pending.extend(sorted((calls & funcs.keys()) - seen))
        for m in re.finditer(r'return\s+false\b|\breject\s*\(|ExitReason::Unsupported', helper['body']):
            pos = helper['start'] + m.start()
            line_start = text.rfind('\n', 0, pos) + 1
            line_end = text.find('\n', pos)
            if line_end < 0:
                line_end = len(text)
            # Keep a small context window: many reject conditions span lines.
            context_start = line_start
            for _ in range(2):
                context_start = text.rfind('\n', 0, max(0, context_start - 1)) + 1
            guards.append({'function': name, 'source': location(WASM, text, pos),
                           'code': text[line_start:line_end].strip(),
                           'context': text[context_start:line_end].strip()})
    return dict(sorted(routes.items())), sorted(guards, key=lambda g: (g['function'], g['source']))


def family(op):
    if op.startswith('A64'):
        return 'A64-only context/memory'
    if op.startswith('A32'):
        if 'Exclusive' in op:
            return 'A32 exclusive memory'
        if 'Memory' in op and ('Read' in op or 'Write' in op):
            return 'A32 ordinary memory'
        if 'Coproc' in op:
            return 'A32 coprocessor'
        return 'A32 context/control/status'
    if op.startswith('FPVector'):
        return 'Vector floating point'
    if op.startswith('Vector') or op in {'ZeroVector', 'Pack2x64To1x128'}:
        return 'Vector integer/data movement'
    if op.startswith('FP'):
        return 'Scalar floating point/conversion'
    if op.startswith('Packed'):
        return 'Packed integer/DSP'
    if 'Saturat' in op:
        return 'Scalar saturation'
    if op.startswith(('AES', 'SHA', 'SM4', 'CRC')):
        return 'Crypto/CRC'
    if op.startswith('Get') and op.endswith('FromOp'):
        return 'Pseudo flags/results'
    return 'Scalar integer/miscellaneous'


CAVEATS = [
    'Static reachability is conservative/heuristic, not proven runtime reachability. '
    'Every source under A32/translate is a root, including helpers and header bodies; '
    'ir.Method calls and IREmitter::Method member pointers seed a method-name closure '
    'through both emitter .cpp files. Opcode references are potential emissions.',
    'All overloads, bit widths and branches of a reached method are unioned. '
    'No argument propagation, decoder/architecture/version filtering, conditional '
    'compilation, FPSCR-mode analysis or optimizer lowering is performed. For example '
    'a 32-bit caller may conservatively pull in 64-bit (or FP16) variants. '
    'Unwitnessed opcodes are not asserted irrelevant; A64-only definitions are not A32 goals.',
    'Not a C++ parser or a guaranteed upper bound: macros, raw strings, aliases, '
    'unrecognized call forms, header-only emitter implementations and externally '
    'introduced/optimized IR can be missed. Unresolved frontend method references '
    'are listed for review. Update parsing/tests if source structure changes.',
    'Dispatch evidence means a top-level instruction() case or positive pre-switch '
    'predicate route, NOT supported semantics. Unrelated case labels, opcode '
    'comparisons and flag-consumer scans are excluded. Rejection-only cases still '
    'count as routes. No support percentage is calculated.',
    'Rejection sites include lexically reached helper bodies and may include '
    'unrelated paths due to name collisions. They are review cues, not exhaustive '
    'per-op preconditions. Operand types, immediates, register ranges, producer '
    'kinds, FPSCR state, terminal shape and region validation can still reject '
    'a block with dispatch evidence for every opcode.',
    'No guest execution, interpreter fallback, proprietary assets, builds or '
    'backend-test changes are needed. Run the separate Python smoke tests. '
    'A runtime JIT-only fixture/semantic test is required to establish actual coverage.',
]


def inventory(root):
    paths = sorted(p for p in (root / TRANSLATE).rglob('*')
                   if p.suffix in {'.cpp', '.h', '.inc'})
    if not paths:
        raise ValueError('no A32 translator sources found')
    files = {p.relative_to(root).as_posix(): p.read_text() for p in paths}
    for path in [OPCODES, WASM] + EMITTERS:
        files[path] = (root / path).read_text()
    defs = parse_opcodes(files[OPCODES])
    evidence, reached, unresolved = reachability(
        {p: files[p] for p in EMITTERS},
        {p.relative_to(root).as_posix(): files[p.relative_to(root).as_posix()] for p in paths}, defs)
    routes, guards = dispatch_inventory(files[WASM], defs)
    missing = sorted(evidence.keys() - routes.keys())
    rows = {}
    for name in sorted(defs):
        rows[name] = {**defs[name], 'family': family(name),
                      'candidate_a32': name in evidence,
                      'witness': evidence.get(name, []),
                      'dispatch': routes.get(name),
                      'candidate_missing': name in evidence and name not in routes}
    return {'schema_version': 1, 'caveats': CAVEATS,
            'inputs': {p: hashlib.sha256(t.encode()).hexdigest() for p, t in sorted(files.items())},
            'counts': {'defined': len(defs), 'candidate_a32': len(evidence),
                       'candidate_with_dispatch': len(evidence.keys() & routes.keys()),
                       'candidate_missing_dispatch': len(missing),
                       'dispatch_routes': len(routes),
                       'unwitnessed_definitions': len(defs.keys() - evidence.keys())},
            'reached_methods': reached, 'unresolved_frontend_methods': unresolved,
            'opcodes': rows, 'rejection_sites': guards}


def markdown(data):
    out = ['# A32 → WASM JIT source inventory', '',
           'Generated by `browser/tests/jit_coverage_inventory.py`. **Heuristic candidates, '
           'not a support/correctness score.**', '', '## Reproduce', '', '```sh',
           'python3 browser/tests/jit_coverage_inventory.py --format markdown --output browser/tests/JIT_COVERAGE_INVENTORY.md',
           'python3 browser/tests/jit_coverage_inventory.py --format json --output /tmp/jit-coverage.json',
           "python3 -m unittest discover -s browser/tests -p 'test_jit_coverage_inventory.py'", '```', '',
           '## Method and limitations', '']
    out.extend('- ' + c for c in data['caveats'])
    out += ['', '## Snapshot', '',
            'Sources may be edited concurrently: regenerate after emitter changes. '
            'The JSON records SHA-256 for every scanned input; this snapshot identifies the key inputs below.', '']
    for path in [OPCODES, WASM] + EMITTERS:
        out.append(f'- `{path}` SHA-256 `{data["inputs"][path]}`')
    out += ['', '| Measure | Count |', '| --- | ---: |']
    for key, count in data['counts'].items():
        out.append(f'| {key} | {count} |')
    out += ['', '## Family breakdown', '',
            '| Family | Candidate A32 | With dispatch evidence | Missing dispatch candidates |',
            '| --- | ---: | ---: | ---: |']
    groups = defaultdict(list)
    for name, row in data['opcodes'].items():
        if row['candidate_a32']:
            groups[row['family']].append((name, row))
    for name, rows in sorted(groups.items()):
        present = sum(r['dispatch'] is not None for _, r in rows)
        out.append(f'| {name} | {len(rows)} | {present} | {len(rows) - present} |')
    out += ['', '## Candidate missing opcodes', '',
            'Every name below has a lexical witness but no recognized dispatch route. '
            'Widths/overloads may be false positives; consult JSON `witness` chains before prioritizing. '
            'These are not claims that any particular application emits them.', '']
    for group, rows in sorted(groups.items()):
        missing = [n for n, r in rows if r['candidate_missing']]
        if missing:
            out += [f'### {group} ({len(missing)})', '', ', '.join('`' + n + '`' for n in missing), '']
    out += ['## Dispatch evidence (not full support)', '',
            '| Opcode | Route | Source |', '| --- | --- | --- |']
    for name, row in data['opcodes'].items():
        if row['dispatch']:
            d = row['dispatch']
            out.append(f'| `{name}` | {d["route"]} | `{d["source"]}` |')
    out += ['', '## Partial-support restrictions: source review checklist', '',
            'The following restrictions are architectural review categories, not a frozen support list. '
            'The live rejection-site extract below is regenerated from the current source.', '',
            '- **Scalar FP:** inspect each dispatched operation for immediate control operands, '
            'fractional-bit/rounding-mode limits, live FPSCR exception enables and location-key FPSCR modes. '
            'A single FP case does not cover all widths, NaN/denormal/exception semantics or conversion modes.',
            '- **Coprocessors:** a route for `A32CoprocGetOneWord` does not imply general CP15 support; '
            'inspect its exact `CoprocessorInfo` allowlist (the TLS-read form in the original snapshot).',
            '- **Pseudo results:** `Get*FromOp` depends on which producer generated the flags/result; '
            'inspect `pseudo()` and producer metadata, not just the pseudo case.',
            '- **Registers/vectors:** S/D/Q type and range checks, architectural footprint and lane '
            'initialization are distinct from vector arithmetic support.',
            '- **Memory/control:** inspect memory argument/descriptor requirements, SVC PC-write '
            'ordering, CheckBit initialization, terminal allowlists and region validation. '
            'Prediction hints may intentionally be no-ops rather than missing execution semantics.', '',
            '### Automatically located instruction/helper rejection sites', '',
            'Sites containing `return false`, `reject(...)` or `ExitReason::Unsupported`; '
            'JSON also includes two preceding lines for multi-line conditions. Terminal/region gates outside '
            'instruction helper closure require separate review.', '',
            '| Function | Source | Source context (up to two preceding lines) |', '| --- | --- | --- |']
    for guard in data['rejection_sites']:
        code = ' '.join(guard['context'].splitlines()).replace('|', '&#124;').replace('`', '&#96;')
        out.append(f'| `{guard["function"]}` | `{guard["source"]}` | `{code}` |')
    out += ['', '## Unresolved frontend method references', '',
            'Often metadata/header-only utilities; not silently promoted to opcode coverage.', '']
    if not data['unresolved_frontend_methods']:
        out.append('None for the recognized reference syntax in this snapshot.')
    for name, sites in data['unresolved_frontend_methods'].items():
        out.append(f'- `{name}`: {len(sites)} reference(s), first `{sites[0]}`')
    return '\n'.join(out) + '\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2],
                        help='repository root (default: relative to this script, not cwd)')
    parser.add_argument('--format', choices=['json', 'markdown'], default='markdown')
    parser.add_argument('--output', type=Path, help='write report instead of stdout')
    args = parser.parse_args(argv)
    try:
        data = inventory(args.root.resolve())
        output = (json.dumps(data, indent=2, sort_keys=True) + '\n'
                  if args.format == 'json' else markdown(data))
        if args.output:
            args.output.write_text(output)
        else:
            sys.stdout.write(output)
    except (OSError, ValueError) as error:
        parser.exit(2, f'inventory error: {error}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
