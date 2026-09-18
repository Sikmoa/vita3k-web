// Lightweight GXM5 contract test: no compiler, Wasm runtime or GPU.
// node --experimental-default-type=module browser/tests/gxm_hle_packet_test.mjs
import assert from 'node:assert/strict';
import { decodeGuestDrawPacket, gxmViewportRect, gxmBlendState, gxmDepthStencilState, gxmVertexFormat }
  from '../web/gxm_hle_bridge.js';

// C++ owns depadding: LINEAR width=4 has align(4,8)*4=32 guest bytes/row.
// GXM5 carries only the packed bytes; it never carries guest pointers/pitch.
const guestPitch = 32, packedPitch = 16;
const guest = new Uint8Array(guestPitch * 4).fill(0xee);
const packed = new Uint8Array(packedPitch * 4);
for (let y = 0; y < 4; ++y) for (let x = 0; x < 4; ++x) {
  const rgba = [x * 40, y * 50, 17, 64 + x + y];
  guest.set(rgba, y * guestPitch + x * 4);
  packed.set(rgba, y * packedPitch + x * 4);
}
const depadded = new Uint8Array(64);
for (let y = 0; y < 4; ++y)
  depadded.set(guest.subarray(y * guestPitch, y * guestPitch + packedPitch), y * packedPitch);
assert.deepEqual(depadded, packed);

// GXM5 fixed words: magic, stride, indexSize, six payload lengths, attribute
// count, blend enabled + seven guest blend words, depth enabled + (four depth
// words and one f32 clear value), texture count, optional eight texture words,
// viewport flat u32, six viewport f32 words, then render info, four words per
// attribute (location, offset, componentCount, guest format), payloads and
// texture bytes.
// Guest blend enums: NONE=0 ADD=1 SUBTRACT=2 REVERSE_SUBTRACT=3 MIN=4 MAX=5;
// factors ZERO=0 ONE=1 SRC_COLOR=2 ... SRC_ALPHA=4 ONE_MINUS_SRC_ALPHA=5 ...
// SRC_ALPHA_SATURATE=10 DST_ALPHA_SATURATE=11. Color mask A=1 R=2 G=4 B=8.
const defaultBlendWords = [0xf, 0, 0, 1, 0, 1, 0];
// Depth: D16=0x02444000 DF32=0x00044000 S8D24=0x01266000;
// funcs NEVER=0 LESS=0x0400000 EQUAL=0x0800000 LESS_EQUAL=0x0c00000
// GREATER=0x1000000 NOT_EQUAL=0x1400000 GREATER_EQUAL=0x1800000 ALWAYS=0x1c00000;
// write mode ENABLED=0 DISABLED=0x00100000; load mode 0=clear.
// Guest attribute formats: U8=0 S8=1 U16=2 S16=3 U8N=4 S8N=5 U16N=6 S16N=7
// F16=8 F32=9 UNTYPED=10.
const f32x4 = [{ location: 0, offset: 0, components: 4, format: 9 }];
function packet({ textured = true, flat = 0, stride = 16, attributes = f32x4,
  vp = [32.5, 16.5, 0.5, 32, -16, 0.5], blend = null, depth = null, clearValue = 1 } = {}) {
  const words = [0x47584d35, stride, 2, 6, 48, 0, 0, 0, 0, attributes.length];
  // The producer enables blending when either channel func is not NONE.
  const blendWords = blend ?? defaultBlendWords;
  const blendEnabledIndex = words.length;
  words.push(blendWords[1] !== 0 || blendWords[2] !== 0 ? 1 : 0, ...blendWords);
  const depthEnabledIndex = words.length;
  words.push(depth ? 1 : 0);
  let clearIndex = -1;
  if (depth) { words.push(...depth); clearIndex = words.length; words.push(0); }
  const textureCountIndex = words.length;
  words.push(textured ? 1 : 0);
  if (textured) words.push(4, 4, 0x0c000000, 0, 1, 2, 0, 64);
  const attributeBytes = attributes.length * 16;
  const bytes = new Uint8Array(words.length * 4 + 4 + 24 + 48 + attributeBytes + 6 + 48
    + (textured ? 64 : 0));
  const view = new DataView(bytes.buffer);
  words.forEach((word, i) => view.setUint32(i * 4, word, true));
  if (depth) view.setFloat32(clearIndex * 4, clearValue, true);
  view.setUint32(words.length * 4, flat, true);
  vp.forEach((value, i) => view.setFloat32(words.length * 4 + 4 + i * 4, value, true));
  // Render info is 48 bytes (12 words) between the viewport and the attributes.
  attributes.forEach((a, i) => [a.location, a.offset, a.components, a.format]
    .forEach((word, w) => view.setUint32(words.length * 4 + 28 + 48 + i * 16 + w * 4, word, true)));
  if (textured) bytes.set(depadded, bytes.length - 64);
  return { bytes, at: { blendEnabledIndex, firstBlendWord: blendEnabledIndex + 1,
    depthEnabledIndex, firstDepthWord: depthEnabledIndex + 1, clearIndex, textureCountIndex,
    firstTextureWord: textureCountIndex + 1, flatWord: words.length,
    firstViewportWord: words.length + 1, firstAttributeWord: words.length + 19 } };
}
const packetBytes = options => packet(options).bytes;

const source = packetBytes();
const decoded = decodeGuestDrawPacket(source);
assert.equal(decoded.stride, 16); // vertex stride, NOT texture row pitch
assert.deepEqual(decoded.attributes, [{ shaderLocation: 0, offset: 0, format: 'float32x4' }]);
assert.deepEqual(decoded.viewport, { flat: false, xOffset: 32.5, yOffset: 16.5,
  zOffset: 0.5, xScale: 32, yScale: -16, zScale: 0.5 });
assert.equal(decodeGuestDrawPacket(packetBytes({ flat: 1 })).viewport.flat, true);
assert.equal(decoded.fragmentTexture.width, 4);
assert.equal(decoded.fragmentTexture.height, 4);
assert.equal(decoded.fragmentTexture.pixels.length / 4, packedPitch);
assert.equal(decoded.fragmentTexture.format, 'rgba8unorm');
assert.deepEqual(decoded.fragmentTexture.pixels, packed);
assert.deepEqual(decoded.fragmentTexture.sampler, { minFilter: 'nearest', magFilter: 'linear',
  addressModeU: 'clamp-to-edge', addressModeV: 'repeat' });
source.fill(0); // decoder output must not alias the packet
assert.deepEqual(decoded.fragmentTexture.pixels, packed);
assert.equal(decodeGuestDrawPacket(packetBytes({ textured: false })).fragmentTexture, undefined);

// No blend or depth state: the guest default still writes all four channels,
// and the pipeline carries no blend or depth-stencil state at all.
assert.equal(decoded.writeMask, 0xf);
assert.equal(decoded.blend, undefined);
assert.equal(decoded.depth, undefined);

// Limbo's observed descriptor: ADD with SRC_ALPHA / ONE_MINUS_SRC_ALPHA on
// both color and alpha, full write mask. The packet carries guest enums; the
// decoder owns the single GXM -> WebGPU translation.
const limboBlend = [0xf, 1, 1, 4, 5, 4, 5];
const limbo = decodeGuestDrawPacket(packetBytes({ blend: limboBlend }));
assert.equal(limbo.writeMask, 0xf);
assert.deepEqual(limbo.blend, {
  color: { operation: 'add', srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha' },
  alpha: { operation: 'add', srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha' } });
// Color mask is guest A=1 R=2 G=4 B=8 -> WebGPU R=1 G=2 B=4 A=8.
assert.equal(decodeGuestDrawPacket(packetBytes({ blend: [0, 0, 0, 1, 0, 1, 0] })).writeMask, 0);
assert.equal(decodeGuestDrawPacket(packetBytes({ blend: [0x2, 0, 0, 1, 0, 1, 0] })).writeMask, 1);
assert.equal(decodeGuestDrawPacket(packetBytes({ blend: [0x4, 0, 0, 1, 0, 1, 0] })).writeMask, 2);
assert.equal(decodeGuestDrawPacket(packetBytes({ blend: [0x8, 0, 0, 1, 0, 1, 0] })).writeMask, 4);
assert.equal(decodeGuestDrawPacket(packetBytes({ blend: [0x1, 0, 0, 1, 0, 1, 0] })).writeMask, 8);
// A single non-NONE channel enables blending, and NONE itself translates as
// ADD exactly like the desktop backends (translate_blend_func).
const mixed = decodeGuestDrawPacket(packetBytes({ blend: [0xf, 0, 5, 1, 0, 2, 9] }));
assert.deepEqual(mixed.blend, {
  color: { operation: 'add', srcFactor: 'one', dstFactor: 'zero' },
  alpha: { operation: 'max', srcFactor: 'src', dstFactor: 'one-minus-dst-alpha' } });

// Every blend func/factor pair the pipeline accepts, plus the invalid shapes.
assert.throws(() => gxmBlendState(0x10, 1, 1, 4, 5, 4, 5), /color mask/);
assert.throws(() => gxmBlendState(0xf, 6, 1, 4, 5, 4, 5), /blend function/);
assert.throws(() => gxmBlendState(0xf, 1, 1, 12, 5, 4, 5), /blend factor/);
assert.throws(() => gxmBlendState(0xf, 1, 1, 4, 11, 4, 5), /blend factor/); // DST_ALPHA_SATURATE
// src-alpha-saturate is the color source factor only.
assert.equal(gxmBlendState(0xf, 1, 1, 10, 0, 1, 0).blend.color.srcFactor, 'src-alpha-saturate');
for (const args of [[0xf, 1, 1, 1, 10, 1, 0], [0xf, 1, 1, 1, 0, 10, 0], [0xf, 1, 1, 1, 0, 1, 10]])
  assert.throws(() => gxmBlendState(...args), /blend factor position/);
assert.deepEqual(gxmBlendState(0xf, 0, 0, 1, 0, 1, 0), { writeMask: 0xf });
const factorNames = ['zero', 'one', 'src', 'one-minus-src', 'src-alpha', 'one-minus-src-alpha',
  'dst', 'one-minus-dst', 'dst-alpha', 'one-minus-dst-alpha', 'src-alpha-saturate'];
for (const func of [1, 2, 3, 4, 5]) for (const [factor, name] of factorNames.entries()) {
  // Source factors only: the saturate factor is invalid in a destination slot,
  // and valid only for the color component.
  const alphaFactor = factor === 10 ? 0 : factor;
  const state = gxmBlendState(0xf, func, func, factor, 0, alphaFactor, 0);
  assert.equal(state.blend.color.operation, ['add', 'subtract', 'reverse-subtract', 'min', 'max'][func - 1]);
  assert.equal(state.blend.alpha.operation, state.blend.color.operation);
  assert.equal(state.blend.color.srcFactor, name);
  assert.equal(state.blend.alpha.srcFactor, factorNames[alphaFactor]);
  assert.deepEqual(state.blend.color.dstFactor, 'zero');
}

// Vertex attributes. Guest format plus component count must select the WebGPU
// vertex format that represents the fetch exactly; the decoder owns that
// translation like the sampler and blend tables.
assert.equal(gxmVertexFormat(9, 1), 'float32');
assert.equal(gxmVertexFormat(9, 3), 'float32x3');
assert.equal(gxmVertexFormat(4, 4), 'unorm8x4');
assert.equal(gxmVertexFormat(5, 2), 'snorm8x2');
assert.equal(gxmVertexFormat(6, 4), 'unorm16x4');
assert.equal(gxmVertexFormat(7, 2), 'snorm16x2');
assert.equal(gxmVertexFormat(8, 4), 'float16x4');
// Limbo's observed attribute: U8N x4 at offset 16 of a 20-byte stride, which
// fills the stride exactly.
assert.deepEqual(decodeGuestDrawPacket(packetBytes({ stride: 20,
  attributes: [{ location: 1, offset: 16, components: 4, format: 4 }] })).attributes,
  [{ shaderLocation: 1, offset: 16, format: 'unorm8x4' }]);
assert.deepEqual(decodeGuestDrawPacket(packetBytes({ stride: 32, attributes: [
  { location: 0, offset: 0, components: 3, format: 9 },
  { location: 5, offset: 12, components: 2, format: 6 }] })).attributes,
  [{ shaderLocation: 0, offset: 0, format: 'float32x3' },
    { shaderLocation: 5, offset: 12, format: 'unorm16x2' }]);
// Non-normalized integer formats (U8/S8/U16/S16) need a scaled vertex format
// WebGPU does not have, UNTYPED needs an integer shader input, and WebGPU has
// no 1- or 3-component 8/16-bit format: all reject instead of reinterpreting
// the guest bytes.
for (const format of [0, 1, 2, 3, 10, 11])
  for (const components of [1, 2, 3, 4])
    assert.throws(() => gxmVertexFormat(format, components), /unsupported vertex attribute format/);
for (const format of [4, 5, 6, 7, 8]) for (const components of [1, 3])
  assert.throws(() => gxmVertexFormat(format, components), /unsupported vertex attribute format/);
for (const format of [4, 5, 6, 7, 8, 9]) for (const components of [0, 5])
  assert.throws(() => gxmVertexFormat(format, components), /unsupported vertex attribute format/);
assert.throws(() => gxmVertexFormat('9', 4), /vertex attribute format and component count must be integers/);
assert.throws(() => gxmVertexFormat(9, 2.5), /vertex attribute format and component count must be integers/);

// Depth-stencil state. Every accepted format is an exact representation of the
// guest layout; the load op is always clear because the native producer rejects
// force_load/force_store.
const limboDepth = decodeGuestDrawPacket(packetBytes({ depth: [0x01266000, 0x0c00000, 0, 0], clearValue: 1 }));
assert.deepEqual(limboDepth.depth, { format: 'depth24plus-stencil8', stencil: true,
  depthCompare: 'less-equal', depthWriteEnabled: true, clearValue: 1 });
assert.deepEqual(gxmDepthStencilState(0x02444000, 0, 0x00100000, 0, 0),
  { format: 'depth16unorm', stencil: false, depthCompare: 'never', depthWriteEnabled: false, clearValue: 0 });
assert.deepEqual(gxmDepthStencilState(0x00044000, 0x1c00000, 0, 0, 0.5),
  { format: 'depth32float', stencil: false, depthCompare: 'always', depthWriteEnabled: true, clearValue: 0.5 });
for (const [func, name] of [[0x0400000, 'less'], [0x0800000, 'equal'], [0x1000000, 'greater'],
  [0x1400000, 'not-equal'], [0x1800000, 'greater-equal']])
  assert.equal(gxmDepthStencilState(0x01266000, func, 0, 0, 1).depthCompare, name);
for (const format of [0x00022000 /* S8 */, 0x00066000 /* DF32_S8 */, 0x000cc000 /* DF32M */,
  0x000ee000 /* DF32M_S8 */, 0])
  assert.throws(() => gxmDepthStencilState(format, 0x0c00000, 0, 0, 1), /depth format/);
assert.throws(() => gxmDepthStencilState(0x01266000, 0x0200000, 0, 0, 1), /depth compare/);
assert.throws(() => gxmDepthStencilState(0x01266000, 0x0c00000, 1, 0, 1), /depth write mode/);
assert.throws(() => gxmDepthStencilState(0x01266000, 0x0c00000, 0, 1, 1), /depth load mode/);
assert.throws(() => gxmDepthStencilState(0x01266000, 0x0c00000, 0, 0, 1.5), /clear value/);
assert.throws(() => gxmDepthStencilState(0x01266000, 0x0c00000, 0, 0, -0.5), /clear value/);
// The clear value is a finite f32 in [0, 1] or the packet rejects.
assert.throws(() => decodeGuestDrawPacket(packetBytes({ depth: [0x01266000, 0x0c00000, 0, 0],
  clearValue: Number.NaN })), /non-finite GXM depth clear value/);
assert.throws(() => decodeGuestDrawPacket(packetBytes({ depth: [0x01266000, 0x0c00000, 0, 0],
  clearValue: 2 })), /clear value/);

// Viewport rect mirrors vulkan sync_viewport_real (res_multiplier 1) plus the
// negative-height normalization; flat covers the full target.
assert.deepEqual(gxmViewportRect({ flat: false, xOffset: 32, yOffset: 16, xScale: 32, yScale: -16 }, 64, 32),
  { x: 0, y: 0, width: 64, height: 32 });
assert.deepEqual(gxmViewportRect(decoded.viewport, 64, 32), { x: 0.5, y: 0.5, width: 64, height: 32 });
assert.deepEqual(gxmViewportRect({ flat: true }, 64, 32), { x: 0, y: 0, width: 64, height: 32 });
assert.deepEqual(gxmViewportRect({ flat: false, xOffset: 10, yOffset: 10, xScale: 5, yScale: 5 }, 64, 32),
  { x: 5, y: 5, width: 10, height: 10 });
assert.throws(() => gxmViewportRect(null, 64, 32), /viewport required/);
assert.throws(() => gxmViewportRect({ flat: false, xOffset: NaN, yOffset: 0, xScale: 1, yScale: 1 }, 64, 32), /finite/);

function changed(index, value) {
  const bytes = packetBytes();
  new DataView(bytes.buffer).setUint32(index * 4, value, true);
  return bytes;
}
const at = packet().at;
// GXM4 and older must fail loudly, not decode with a shifted layout.
for (const [index, value] of [[0, 0x47584d34], [0, 0x47584d31], [0, 0x47584d32],
  [at.textureCountIndex, 2], [at.firstTextureWord, 0], [at.firstTextureWord, 4097],
  [at.firstTextureWord + 1, 0], [at.firstTextureWord + 2, 0], [at.firstTextureWord + 3, 2],
  [at.firstTextureWord + 4, 3], [at.firstTextureWord + 5, 3], [at.firstTextureWord + 6, 7],
  [at.firstTextureWord + 7, 63], [at.flatWord, 2],
  // Blend: enable flag inconsistent with the descriptor, and out-of-range words.
  [at.blendEnabledIndex, 2], [at.blendEnabledIndex, 1],
  [at.firstBlendWord, 0x10], [at.firstBlendWord + 1, 6], [at.firstBlendWord + 3, 12],
  // Depth: enable flag value.
  [at.depthEnabledIndex, 2],
  // Attributes: a U8 format and an out-of-range component count both reject.
  [at.firstAttributeWord + 3, 0], [at.firstAttributeWord + 2, 5],
  [at.firstAttributeWord + 2, 0]])
  assert.throws(() => decodeGuestDrawPacket(changed(index, value)),
    `word ${index}=${String(value)} must reject`);
assert.throws(() => decodeGuestDrawPacket(packetBytes({ depth: [0x01266000, 0x0c00000, 0, 1] })),
  /depth load mode/);
assert.throws(() => decodeGuestDrawPacket(packetBytes({ depth: [0x01266000, 0x0c00000, 1, 0] })),
  /depth write mode/);
// The depth block only exists when the enable flag says so.
assert.throws(() => decodeGuestDrawPacket(changed(at.depthEnabledIndex, 1)), /truncated|depth format/);
assert.throws(() => decodeGuestDrawPacket(changed(at.blendEnabledIndex, 1)), /blend flag/);

assert.throws(() => decodeGuestDrawPacket(changed(at.firstViewportWord, 0x7fc00000)), /non-finite/);
for (let n = 0; n < packetBytes().length; ++n)
  assert.throws(() => decodeGuestDrawPacket(packetBytes().slice(0, n)), /truncated/);
assert.throws(() => decodeGuestDrawPacket(new Uint8Array([...packetBytes(), 0])), /trailing/);
for (const min of [0,1]) for (const mag of [0,1]) for (const u of [0,1,2]) for (const v of [0,1,2]) {
  const words = [0x47584d35, 16, 2, 6, 48, 0, 0, 0, 0, 1];
  words.push(0, ...defaultBlendWords, 0, 1);
  words.push(4, 4, 0x0c000000, min, mag, u, v, 64);
  const bytes = new Uint8Array(words.length * 4 + 4 + 24 + 48 + 16 + 6 + 48 + 64);
  const view = new DataView(bytes.buffer);
  words.forEach((word, i) => view.setUint32(i * 4, word, true));
  // One F32x4 attribute: location 0, offset 0, four components, format 9.
  view.setUint32(words.length * 4 + 28 + 48 + 2 * 4, 4, true);
  view.setUint32(words.length * 4 + 28 + 48 + 3 * 4, 9, true);
  bytes.set(depadded, bytes.length - 64);
  const sampler = decodeGuestDrawPacket(bytes).fragmentTexture.sampler;
  assert.equal(sampler.minFilter, ['nearest','linear'][min]);
  assert.equal(sampler.magFilter, ['nearest','linear'][mag]);
  assert.equal(sampler.addressModeU, ['repeat','mirror-repeat','clamp-to-edge'][u]);
  assert.equal(sampler.addressModeV, ['repeat','mirror-repeat','clamp-to-edge'][v]);
}
console.log('GXM5 packet: 4x4 ABGR8, 32-byte guest / 16-byte packed rows, owned pixels, viewport words/rects, 36 samplers, blend translation, depth-stencil translation, vertex-format translation, rejection checks passed');
