import assert from 'node:assert/strict';
import fs from 'node:fs';
import { test } from 'node:test';
import { generate, parseCanonical, parseDatabase } from '../generate_known_nids.mjs';

const source = `# fixture
version: 2
firmware: 3.60
modules:
  Test:
    nid: 0x01
    libraries:
      First:
        nid: 0x02
        kernel: false
        stubname: Test
        functions:
          original: 0x10
          new_function: 0xFFFFFFFF
          #ignored: 0xCAFE
        variables:
          new_variable: 0x20
      Second:
        kernel: true
        nid: 0x03
        functions:
          alias: 0x10
          other_function: 0xFFFFFFFF
`;
const canonical = 'NID(original, 0x10)\nVAR_NID(old_variable, 0x30)\n';

test('functions, variables, comments and library aliases are preserved', () => {
    const result = generate(source, canonical);
    assert.equal(parseDatabase(source).length, 5);
    assert.deepEqual(result.summary, {
        sourceRecords: 5, sourceUniqueNids: 3, canonicalNids: 2,
        additionalNames: 2, libraryAliases: 2, conflictingLibraryPairs: 0,
        totalRecognizedNids: 4,
    });
    assert.match(result.known, /KNOWN_NID\("new_function", 0xFFFFFFFF\)/);
    assert.match(result.known, /KNOWN_NID\("new_variable", 0x00000020\)/);
    assert.doesNotMatch(result.known, /ignored/);
    assert.match(result.aliases, /NID_ALIAS\("alias", 0x00000010, 0x00000003\)/);
    assert.match(result.aliases, /NID_ALIAS\("other_function", 0xFFFFFFFF, 0x00000003\)/);
});

test('conflicting source names retain the existing canonical spelling', () => {
    const conflicting = source.replace('          original:', '          conflict:').replace('          new_function:', '          original: 0x10\n          new_function:');
    const result = generate(conflicting, canonical);
    assert.equal(result.summary.conflictingLibraryPairs, 1);
    assert.match(result.aliases, /Upstream conflict:.*conflict, original/);
    assert.doesNotMatch(result.aliases, /NID_ALIAS\("conflict"/);
});

test('unsupported or incomplete input fails instead of silently losing NIDs', () => {
    assert.throws(() => parseDatabase(source.replace('0xFFFFFFFF', '4294967295')), /Unsupported database syntax/);
    assert.throws(() => parseDatabase(source.replace('        nid: 0x02\n', '')), /Incomplete/);
    assert.throws(() => parseDatabase(source.replace('version: 2', 'version: 3')), /Unsupported/);
    assert.throws(() => parseCanonical(canonical + 'NID(duplicate, 0x10)\n'), /Duplicate/);
});

test('checked-in supplemental names never overlap canonical dispatch IDs', () => {
    const base = new URL('../include/nids/', import.meta.url);
    const existing = parseCanonical(fs.readFileSync(new URL('nids.inc', base), 'utf8'));
    const known = fs.readFileSync(new URL('known_nids.inc', base), 'utf8');
    const all = new Set(existing.keys());
    let count = 0;
    for (const match of known.matchAll(/^KNOWN_NID\("\w+", (0x[\dA-F]{8})\)$/gm)) {
        assert.equal(all.has(Number(match[1])), false, `duplicate NID ${match[1]}`);
        all.add(Number(match[1]));
        count++;
    }
    assert.equal(count, 2150);
});
