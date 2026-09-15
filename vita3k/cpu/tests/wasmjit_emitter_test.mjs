// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
// node vita3k/cpu/tests/wasmjit_emitter_test.mjs <fixture-directory>
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {join} from 'node:path';

const directory = process.argv[2];
const fixtures = JSON.parse(readFileSync(join(directory, 'cases.json'), 'utf8'));
const memory = new WebAssembly.Memory({initial: 2});
const bytes = new Uint8Array(memory.buffer);
// Helper contract (wasm_jit_cpu.cpp checked_memory_read/write): reads zero
// memory_value[0..3] then fill the addressed guest bytes little-endian;
// writes consume bytes across all four words, so 8-byte transfers use two.
// The fast-path fallback encodes a reason in the width's high byte; mask it.
const mem_read = (stateOffset, address, rawWidth) => {
    address >>>= 0; // Wasm i32 arguments arrive in JavaScript as signed numbers.
    const view = new DataView(memory.buffer);
    const width = rawWidth & 0xff;
    if (address + width > bytes.length) {
        view.setUint32(stateOffset + 88, address >>> 0, true);
        view.setUint32(stateOffset + 92, 0, true);
        return 2;
    }
    for (let word = 0; word < 4; ++word)
        view.setUint32(stateOffset + 96 + 4 * word, 0, true);
    for (let i = 0; i < width; ++i) {
        const word = view.getUint32(stateOffset + 96 + 4 * (i >> 2), true);
        view.setUint32(stateOffset + 96 + 4 * (i >> 2), word | (bytes[address + i] << ((i & 3) * 8)), true);
    }
    return 0;
};
const mem_write = (stateOffset, address, rawWidth) => {
    address >>>= 0;
    const view = new DataView(memory.buffer);
    const width = rawWidth & 0xff;
    if (address + width > bytes.length) {
        view.setUint32(stateOffset + 88, address >>> 0, true);
        view.setUint32(stateOffset + 92, 1, true);
        return 2;
    }
    for (let i = 0; i < width; ++i) {
        const word = view.getUint32(stateOffset + 96 + 4 * (i >> 2), true);
        bytes[address + i] = word >>> ((i & 3) * 8);
    }
    return 0;
};
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
let regionRuns = 0;
for (const fixture of fixtures) {
    const isRegion = fixture.budget !== undefined;
    const bytes = readFileSync(join(directory, `${fixture.name}.wasm`));
    assert(WebAssembly.validate(bytes), `${fixture.name}: module must validate`);
    const module = new WebAssembly.Module(bytes);
    assert.deepEqual(WebAssembly.Module.imports(module), [
        {module: 'env', name: 'memory', kind: 'memory'},
        {module: 'env', name: 'mem_read', kind: 'function'},
        {module: 'env', name: 'mem_write', kind: 'function'}]);
    const exportName = isRegion ? 'run' : 'block';
    assert.deepEqual(WebAssembly.Module.exports(module), [{name: exportName, kind: 'function'}]);
    const block = new WebAssembly.Instance(module, {env: {memory, mem_read, mem_write}}).exports[exportName];
    // Wasm export is an actual typed function, not a JS trampoline.
    let invoke;
    if (isRegion) {
        invoke = offset => block(offset, fixture.budget);
    } else {
        const table = new WebAssembly.Table({initial: 1, element: 'anyfunc'});
        table.set(0, block);
        invoke = new WebAssembly.Instance(indirectModule, {env: {table}}).exports.call;
    }
    for (const [index, test] of fixture.cases.entries()) {
        for (const offset of [0x400, 0x10404]) {
            const whole = new Uint32Array(memory.buffer);
            const view = new Uint32Array(memory.buffer, offset, test.in.length);
            whole.fill(0xcafebabe);
            view.set(test.in);
            // Guest-memory preconditions (loads): seed after the fill so only
            // these words differ from the 0xcafebabe background.
            if (test.pre)
                for (const [address, word] of Object.entries(test.pre))
                    new DataView(memory.buffer).setUint32(Number(address), word >>> 0, true);
            const label = `${fixture.name} case ${index} @${offset}`;
            assert.equal(test.in.length, test.out.length, `${label}: state size`);
            const reason = invoke(offset);
            assert.equal(reason >>> 0, test.out[19], `${label}: reason`);
            assert.deepEqual(Array.from(view), test.out, `${label}: state`);
            assert.equal(whole[offset / 4 - 1], 0xcafebabe, `${label}: leading canary`);
            assert.equal(whole[offset / 4 + view.length], 0xcafebabe, `${label}: trailing canary`);
            // Guest-memory expectations: checked AFTER execution so helper-
            // backed stores must have landed at the addressed guest bytes.
            if (test.mem) {
                const words = new DataView(memory.buffer);
                for (const [address, word] of Object.entries(test.mem))
                    assert.equal(words.getUint32(Number(address), true), word,
                        `${label}: guest memory @${address}`);
            }
            if (isRegion) ++regionRuns;
            else ++runs;
        }
    }
    assert.throws(() => invoke(memory.buffer.byteLength - 4), WebAssembly.RuntimeError);
}
console.log(`Wasm execution passed: ${fixtures.length} modules, ${runs} call_indirect calls, ${regionRuns} region calls (two state offsets), table insertion and OOB traps`);
