// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
// node vita3k/cpu/tests/wasmjit_emitter_test.mjs <fixture-directory> <fp-helper.wasm>
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {join} from 'node:path';
import {WASI} from 'node:wasi';

const directory = process.argv[2];
const fixtures = JSON.parse(readFileSync(join(directory, 'cases.json'), 'utf8'));
// Guest-write epoch table (JitState.write_epochs_base, word 116): each inline
// fast-path store writes JitState.write_epoch at base + (guest address >> 12)
// * 4. This runner owns the scratch table the generator names (kEpochTable in
// wasmjit_emitter_test.cpp), above the 128 KiB guest window the checked
// helpers serve; it covers guest pages below 2^15, and a store past it traps
// instead of landing in fixture data.
const guestBytes = 0x20000;
const epochTable = guestBytes, epochTableBytes = 0x20000;
const memory = new WebAssembly.Memory({initial: (epochTable + epochTableBytes) / 65536});
const bytes = new Uint8Array(memory.buffer);
const wasi = new WASI({version: 'preview1', args: [], env: {}, preopens: {}});
const exactFPInstance = process.argv[3] ? new WebAssembly.Instance(
    new WebAssembly.Module(readFileSync(process.argv[3])), {
        wasi_snapshot_preview1: wasi.wasiImport,
        env: {abort() { throw new Error('FP helper aborted'); }},
    }) : null;
if (exactFPInstance) wasi.initialize(exactFPInstance);
const exactFP = exactFPInstance?.exports;
// Helper contract (wasm_jit_cpu.cpp checked_memory_read/write): reads zero
// memory_value[0..3] then fill the addressed guest bytes little-endian;
// writes consume bytes across all four words, so 8-byte transfers use two.
// The fast-path fallback encodes a reason in the width's high byte; mask it.
const mem_read = (stateOffset, address, rawWidth) => {
    address >>>= 0; // Wasm i32 arguments arrive in JavaScript as signed numbers.
    const view = new DataView(memory.buffer);
    const width = rawWidth & 0xff;
    if (address + width > guestBytes) {
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
    if (address + width > guestBytes) {
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
// Native fp64 helper contract (wasm_jit_cpu.cpp fp64_helper): operation
// 0/1/2/3 = add/sub/mul/div on i64 pairs packed from memory_value[0..3];
// result bits go back to memory_value[0..1] and the returned flags are ORed
// into FPSCR by the generated code. Fixtures only exercise the operation
// selection shape, so ordinary JS binary64 arithmetic suffices; x/0 raises
// IOC with the default quiet NaN like the native helper.
const fp64 = (stateOffset, operation, fpscr) => {
    const view = new DataView(memory.buffer);
    if (operation & 0x10000) {
        assert.ok(exactFP, 'extended FP fixtures require the compiled helper Wasm as argv[3]');
        const base = stateOffset + 468;
        const result = exactFP.extended_fp(operation, view.getBigUint64(base, true),
            view.getBigUint64(base + 8, true), view.getBigUint64(base + 16, true), fpscr);
        view.setBigUint64(base, result, true);
        return exactFP.extended_flags();
    }
    const low = word => view.getUint32(stateOffset + 96 + 4 * word, true);
    const a = low(0) + low(1) * 0x100000000, b = low(2) + low(3) * 0x100000000;
    const box = new DataView(new Float64Array(1).buffer);
    box.setBigUint64(0, BigInt(a), true);
    const fa = box.getFloat64(0, true);
    box.setBigUint64(0, BigInt(b), true);
    const fb = box.getFloat64(0, true);
    if (operation === 2 && fb === 0) {
        view.setUint32(stateOffset + 96, 0, true);
        view.setUint32(stateOffset + 100, 0x7ff80000, true);
        return 1;
    }
    const result = operation === 0 ? fa + fb : operation === 1 ? fa - fb
        : operation === 2 ? fa * fb : fa / fb;
    box.setFloat64(0, result, true);
    const bits = box.getBigUint64(0, true);
    view.setUint32(stateOffset + 96, Number(bits & 0xffffffffn), true);
    view.setUint32(stateOffset + 100, Number(bits >> 32n), true);
    return 0;
};
for (const fixture of fixtures) {
    const isRegion = fixture.budget !== undefined;
    const bytes = readFileSync(join(directory, `${fixture.name}.wasm`));
    assert(WebAssembly.validate(bytes), `${fixture.name}: module must validate`);
    const module = new WebAssembly.Module(bytes);
    assert.deepEqual(WebAssembly.Module.imports(module), [
        {module: 'env', name: 'memory', kind: 'memory'},
        {module: 'env', name: 'mem_read', kind: 'function'},
        {module: 'env', name: 'mem_write', kind: 'function'},
        {module: 'env', name: 'fp64', kind: 'function'}]);
    const exportName = isRegion ? 'run' : 'block';
    assert.deepEqual(WebAssembly.Module.exports(module), [{name: exportName, kind: 'function'}]);
    const block = new WebAssembly.Instance(module, {env: {memory, mem_read, mem_write, fp64}}).exports[exportName];
    const variants = (fixture.variants ?? []).map(filename => {
        const raw = readFileSync(join(directory, filename));
        assert(WebAssembly.validate(raw), `${filename}: module must validate`);
        const candidate = new WebAssembly.Module(raw);
        assert.deepEqual(WebAssembly.Module.imports(candidate), WebAssembly.Module.imports(module));
        assert.deepEqual(WebAssembly.Module.exports(candidate), WebAssembly.Module.exports(module));
        return {filename, run: new WebAssembly.Instance(candidate,
            {env: {memory, mem_read, mem_write, fp64}}).exports.run};
    });
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
            const reset = () => {
                whole.fill(0xcafebabe);
                view.set(test.in);
                // Seed guest memory after resetting the background/state.
                if (test.pre)
                    for (const [address, word] of Object.entries(test.pre))
                        new DataView(memory.buffer).setUint32(Number(address), word >>> 0, true);
            };
            reset();
            const label = `${fixture.name} case ${index} @${offset}`;
            assert.equal(test.in.length, test.out.length, `${label}: state size`);
            assert.equal(test.in[116], epochTable, `${label}: write epoch table`);
            const reason = invoke(offset);
            if (!fixture.differential) {
                assert.equal(reason >>> 0, test.out[19], `${label}: reason`);
                assert.deepEqual(Array.from(view), test.out, `${label}: state`);
            }
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
            // Compare ALL JitState bytes and the complete test memory image,
            // including guest stores, helper scratch, counters and canaries.
            // Fresh inputs for every call also exercise spill/reload across
            // invocation boundaries. No candidate becomes another's oracle.
            if (variants.length) {
                const reference = whole.slice();
                // _F variants (assume_fast_bases) carry the host's proof
                // obligation: all three fast-path bases nonzero (JitState
                // words 95/96/100). Inputs violating it are out of contract
                // (the emitter drops the enabled guard); skip, don't fail.
                const fastContract = test.in[95] !== 0 && test.in[96] !== 0 && test.in[100] !== 0;
                for (const variant of variants) {
                    if (variant.filename.includes('_F') && !fastContract) continue;
                    reset();
                    assert.equal(variant.run(offset, fixture.budget), reason,
                        `${label} ${variant.filename}: differential reason`);
                    assert.deepEqual(whole, reference,
                        `${label} ${variant.filename}: differential memory/state`);
                    ++regionRuns;
                }
            }
            if (isRegion) ++regionRuns;
            else ++runs;
        }
    }
    assert.throws(() => invoke(memory.buffer.byteLength - 4), WebAssembly.RuntimeError);
    for (const variant of variants)
        assert.throws(() => variant.run(memory.buffer.byteLength - 4, fixture.budget), WebAssembly.RuntimeError);
}
console.log(`Wasm execution passed: ${fixtures.length} reference fixtures plus P/K/PK variants, ${runs} call_indirect calls, ${regionRuns} region calls (two state offsets), table insertion and OOB traps`);

const aotState = JSON.parse(readFileSync(join(directory, 'portable_aot_state.json'), 'utf8'));
const aotLut = 0x8000;
for (let mode = 0; mode < 4; ++mode) {
    const module = new WebAssembly.Module(readFileSync(join(directory, `portable_aot_${mode}.wasm`)));
    const {entry} = new WebAssembly.Instance(module,
        {env: {memory, mem_read, mem_write, fp64, aot_lut: aotLut}}).exports;
    for (const offset of [0x400, 0x10404]) {
        new Uint32Array(memory.buffer).fill(0xcafebabe);
        const state = new Uint32Array(memory.buffer, offset, aotState.length);
        state.set(aotState);
        new Uint32Array(memory.buffer, aotLut, 2).set([1, 0]); // slot 0, member 0, ARM
        assert.equal(entry(offset, 10), 1, `AOT mode ${mode}: SVC exit`);
        assert.equal(state[5], 0xa8800000, 'AOT fused cancellation result');
        const value = new DataView(memory.buffer).getFloat64(offset + 6 * 4, true);
        assert.equal(value, 0xffffffff / 65536, 'AOT direct FPFixedU32ToDouble');
        assert.equal(state[15], 0x1004, 'AOT next PC');
        assert.equal(state[17], 0, 'AOT FPSCR');
        assert.equal(state[18], 42, 'AOT SVC immediate');
        // Region/AOT accounting ACCUMULATES into state.executed (REGION_ABI.md:
        // "state.executed accumulates monotonically across calls"; the host
        // reads the per-call delta), unlike single-block emission, which
        // overwrites it. The seed state starts at 99, so exactly one executed
        // instruction must land at 100. Assert the delta and the sum so neither
        // an overwrite nor an extra tick can hide.
        assert.equal((state[20] - aotState[20]) >>> 0, 1, 'AOT executed ticks delta');
        assert.equal(state[20], (aotState[20] + 1) >>> 0, 'AOT executed ticks accumulate');
        assert.deepEqual(Array.from(state.slice(117, 123)), aotState.slice(117, 123), 'AOT FP scratch preservation');
        const whole = new Uint32Array(memory.buffer);
        assert.equal(whole[offset / 4 - 1], 0xcafebabe, 'AOT leading canary');
        assert.equal(whole[offset / 4 + state.length], 0xcafebabe, 'AOT trailing canary');
    }
}
console.log('AOT execution passed: direct fixed conversion and fused FP helper under all four state policies');
