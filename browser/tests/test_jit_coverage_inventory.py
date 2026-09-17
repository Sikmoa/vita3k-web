"""Source-only unit/smoke tests; never invokes guest code, builds or backend tests."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import jit_coverage_inventory as inventory


class ParserTests(unittest.TestCase):
    def test_opcode_macros_and_comments(self):
        definitions = inventory.parse_opcodes('''
// OPCODE(Fake, Void,)
/* A32OPC(NotReal, U32,) */
OPCODE(Add32, U32, U32, U32, U1)
A32OPC(GetRegister, U32, A32Reg)
A64OPC(GetW, U32, A64Reg)
''')
        self.assertEqual(set(definitions), {'Add32', 'A32GetRegister', 'A64GetW'})
        self.assertEqual(definitions['A32GetRegister']['line'], 5)
        self.assertEqual(definitions['Add32']['types'], ['U32', 'U32', 'U32', 'U1'])
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            inventory.parse_opcodes('OPCODE(A,Void,)\nOPCODE(A,Void,)')

    def test_balanced_bodies_ignore_literals_and_nested_lambdas(self):
        text = '''U32 IREmitter::Foo(void (*fn)(u64)) {
    const char *s = "} Opcode::Fake";
    // } Opcode::Fake
    auto nested = [&] { return Bar(); };
    return Inst(Opcode::Real);
}
U32 IREmitter::Bar() const { return Inst(Opcode::Other); }
'''
        functions = inventory.functions(text, qualified=True)
        self.assertEqual([f['name'] for f in functions], ['Foo', 'Bar'])
        self.assertEqual([n for n, _ in inventory.opcode_refs(functions[0]['body'])], ['Real'])
        with self.assertRaisesRegex(ValueError, 'unbalanced'):
            inventory.balanced_end('{', 0)

    def test_reachability_overloads_cycles_pointer_roots(self):
        # Foo->Bar->Foo terminates. Both Foo overloads remain candidates,
        # even though the frontend only passes 32-bit values. Uncalled excluded.
        sources = {'generic.cpp': '''
U32 IREmitter::Foo(U32 a) { Bar(); return Inst(Opcode::Narrow); }
U64 IREmitter::Foo(U64 a) { return Inst(Opcode::Wide); }
void IREmitter::Bar() { Foo(); Inst(Opcode::Indirect); }
void IREmitter::Ptr() { Inst(Opcode::Pointer); }
void IREmitter::Unused() { Inst(Opcode::Uncalled); }
'''}
        translations = {'translate.cpp': '''
void Visit() { v.ir.Foo(value); helper(&IR::IREmitter::Ptr);
 ir.Unknown(); /* ir.Unused(); */ }
'''}
        defs = {n: {} for n in ['Narrow', 'Wide', 'Indirect', 'Pointer', 'Uncalled']}
        evidence, reached, unresolved = inventory.reachability(sources, translations, defs)
        self.assertEqual(set(evidence), {'Narrow', 'Wide', 'Indirect', 'Pointer'})
        self.assertEqual(reached, ['Bar', 'Foo', 'Ptr'])
        self.assertEqual(set(unresolved), {'Unknown'})
        self.assertIn('translate.cpp:2', evidence['Indirect'][0])
        self.assertTrue(any('IREmitter::Bar' in step for step in evidence['Indirect']))

    def test_dispatch_routes_not_global_references(self):
        source = '''
bool arithmetic(Op op) { return op == Op::Add32 || op == Op::Sub32; }
bool shift(Op op) { return op == Op::Shift32; }
bool scan() { switch (x) { case Op::ScanOnly: return true; } }
bool helper() { if (x) return false; return true; }
bool instruction(const Inst &inst) {
 const auto kind = inst.GetOpcode();
 if (arithmetic(kind)) { helper(); return ok; }
 if (shift(kind)) { helper(); return ok; }
 switch (kind) {
 case Op::A:
 case Op::B: { switch (other) { case Op::Nested: return false; } break; }
 case Op::Rejected: return false;
 // case Op::Comment: return true;
 default: return false;
 }
 const bool unused_comparison = kind == Op::Comparison;
 return true;
}
'''
        defs = {n: {} for n in ['A', 'B', 'Rejected', 'Add32', 'Sub32', 'Shift32']}
        routes, guards = inventory.dispatch_inventory(source, defs)
        self.assertEqual(set(routes), set(defs))
        self.assertEqual(routes['Add32']['route'], 'positive predicate arithmetic')
        self.assertIn('Rejected', routes)  # A route is expressly NOT support.
        self.assertTrue(any(g['function'] == 'helper' for g in guards))
        self.assertFalse(any(g['function'] == 'scan' for g in guards))
        with self.assertRaisesRegex(ValueError, 'unrecognized dispatch predicate'):
            inventory.dispatch_inventory(source.replace('op == Op::Add32', 'op != Op::Add32'), defs)

    def test_no_silent_empty_inventory(self):
        with self.assertRaisesRegex(ValueError, 'no OPCODE'):
            inventory.parse_opcodes('// nothing')
        with self.assertRaisesRegex(ValueError, 'missing WASM'):
            inventory.dispatch_inventory('bool other() {}', {})

    def test_family_precedence(self):
        self.assertEqual(inventory.family('FPVectorAdd32'), 'Vector floating point')
        self.assertEqual(inventory.family('A32ExclusiveReadMemory64'), 'A32 exclusive memory')
        self.assertEqual(inventory.family('VectorSignedSaturatedAdd32'), 'Vector integer/data movement')


class RepositorySmokeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parents[2]
        cls.data = inventory.inventory(cls.root)

    def test_partition_and_witness_invariants_not_frozen_counts(self):
        data = self.data
        rows = data['opcodes']
        candidates = {n for n, r in rows.items() if r['candidate_a32']}
        routes = {n for n, r in rows.items() if r['dispatch']}
        missing = {n for n, r in rows.items() if r['candidate_missing']}
        self.assertEqual(missing, candidates - routes)
        self.assertEqual(data['counts']['candidate_a32'], len(candidates))
        self.assertEqual(data['counts']['candidate_with_dispatch'], len(candidates & routes))
        self.assertEqual(data['counts']['candidate_missing_dispatch'], len(missing))
        self.assertTrue(candidates)
        self.assertTrue(routes)
        self.assertFalse(any(n.startswith('A64') for n in candidates))
        self.assertIn('Add32', candidates & routes)  # Not a switch case.
        self.assertIn('FPAdd64', candidates)  # Generic width union is intentional.
        self.assertNotIn('Void', candidates)  # Optimizer/dead marker != translation root.
        for n in candidates:
            self.assertTrue(rows[n]['witness'], n)
        self.assertIn(inventory.WASM, data['inputs'])

    def test_markdown_and_json_roundtrip(self):
        text = inventory.markdown(self.data)
        self.assertIn('conservative/heuristic', text)
        self.assertIn('not full support', text)
        self.assertIn('Partial-support restrictions', text)
        self.assertEqual(json.loads(json.dumps(self.data)), self.data)
        for name, row in self.data['opcodes'].items():
            if row['candidate_missing']:
                self.assertIn('`' + name + '`', text)

    def test_cli_from_different_cwd_and_actionable_error(self):
        script = self.root / 'browser/tests/jit_coverage_inventory.py'
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'report.json'
            proc = subprocess.run([sys.executable, str(script), '--format', 'json',
                                   '--output', str(path)], cwd=temp, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(json.loads(path.read_text())['schema_version'], 1)
            proc = subprocess.run([sys.executable, str(script), '--root', temp],
                                  cwd=temp, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 2)
            self.assertIn('inventory error: no A32 translator sources', proc.stderr)
            self.assertNotIn('Traceback', proc.stderr)


if __name__ == '__main__':
    unittest.main()
