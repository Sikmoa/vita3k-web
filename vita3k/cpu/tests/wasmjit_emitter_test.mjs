// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
// node vita3k/cpu/tests/wasmjit_emitter_test.mjs <fixture-directory>
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {join} from 'node:path';

const directory = process.argv[2];
const fixtures = JSON.parse(readFileSync(join(directory, 'cases.json'), 'utf8'));
const memory = new WebAssembly.Memory({initial: 2});
// Import a funcref table, then invoke slot 0 via an actual Wasm call_indirect.
// Type 0 is (i32)->i32, the same signature used by the generated export.
const indirectModule = new WebAssembly.Module(Uint8Array.from([
    0, 97, 115, 109, 1, 0, 0, 0,
    1, 6, 1, 96, 1, 127, 1, 127,
    2, 15, 1, 3, 101, 110, 118, 5, 116, 97, 98, 108, 101, 1, 112, 0, 1,
    3, 2, 1, 0,
    7, 8, 1, 4, 99, 97, 108, 108, 0, 0,
    10, 11, 1, 9, 0, 32, 0, 65, 0, 17, 0, 0, 11,
]));
let runs = 0;
for (const fixture of fixtures) {
    const bytes = readFileSync(join(directory, `${fixture.name}.wasm`));
    assert(WebAssembly.validate(bytes), `${fixture.name}: module must validate`);
    const module = new WebAssembly.Module(bytes);
    assert.deepEqual(WebAssembly.Module.imports(module), [{module: 'env', name: 'memory', kind: 'memory'}]);
    assert.deepEqual(WebAssembly.Module.exports(module), [{name: 'block', kind: 'function'}]);
    const {exports: {block}} = new WebAssembly.Instance(module, {env: {memory}});
    // Wasm export is an actual typed function, not a JS trampoline.
    const table = new WebAssembly.Table({initial: 1, element: 'anyfunc'});
    table.set(0, block);
    const indirect = new WebAssembly.Instance(indirectModule, {env: {table}}).exports.call;
    for (const [index, test] of fixture.cases.entries()) {
        for (const offset of [0x400, 0x10404]) {
            const whole = new Uint32Array(memory.buffer);
            const view = new Uint32Array(memory.buffer, offset, 21);
            whole.fill(0xcafebabe);
            view.set(test.in);
            const reason = indirect(offset);
            const label = `${fixture.name} case ${index} @${offset}`;
            assert.equal(reason >>> 0, test.out[19], `${label}: reason`);
            assert.deepEqual(Array.from(view), test.out, `${label}: state`);
            assert.equal(whole[offset / 4 - 1], 0xcafebabe, `${label}: leading canary`);
            assert.equal(whole[offset / 4 + 21], 0xcafebabe, `${label}: trailing canary`);
            ++runs;
        }
    }
    assert.throws(() => block(memory.buffer.byteLength - 4), WebAssembly.RuntimeError);
}
console.log(`Wasm execution passed: ${fixtures.length} modules, ${runs} call_indirect calls (two state offsets), table insertion and OOB traps`);
