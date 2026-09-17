// Lightweight GXM2 contract test: no compiler, Wasm runtime or GPU.
// node --experimental-default-type=module browser/tests/gxm_hle_packet_test.mjs
import assert from 'node:assert/strict';
import { decodeGuestDrawPacket } from '../web/gxm_hle_bridge.js';

// C++ owns depadding: LINEAR width=4 has align(4,8)*4=32 guest bytes/row.
// GXM2 carries only the packed bytes; it never carries guest pointers/pitch.
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
function packet(textured = true) {
  const words = [0x47584d32, 16, 2, 6, 48, 0, 0, 0, 0, 1, textured ? 1 : 0];
  if (textured) words.push(4, 4, 0x0c000000, 0, 1, 2, 0, 64);
  const bytes = new Uint8Array(words.length * 4 + 48 + 12 + 6 + 48 + (textured ? 64 : 0));
  const view = new DataView(bytes.buffer);
  words.forEach((word, i) => view.setUint32(i * 4, word, true));
  view.setUint32(words.length * 4 + 48 + 8, 4, true); // attribute components
  if (textured) bytes.set(depadded, bytes.length - 64);
  return bytes;
}
const source = packet();
const decoded = decodeGuestDrawPacket(source);
assert.equal(decoded.stride, 16); // vertex stride, NOT texture row pitch
assert.equal(decoded.fragmentTexture.width, 4);
assert.equal(decoded.fragmentTexture.height, 4);
assert.equal(decoded.fragmentTexture.pixels.length / 4, packedPitch);
assert.equal(decoded.fragmentTexture.format, 'rgba8unorm');
assert.deepEqual(decoded.fragmentTexture.pixels, packed);
assert.deepEqual(decoded.fragmentTexture.sampler, { minFilter: 'nearest', magFilter: 'linear',
  addressModeU: 'clamp-to-edge', addressModeV: 'repeat' });
source.fill(0); // decoder output must not alias the packet
assert.deepEqual(decoded.fragmentTexture.pixels, packed);
assert.equal(decodeGuestDrawPacket(packet(false)).fragmentTexture, undefined);
function changed(word, value) {
  const bytes = packet(); new DataView(bytes.buffer).setUint32(word * 4, value, true); return bytes;
}
for (const [word, value] of [[0,0x47584d31], [10,2], [11,0], [11,4097], [12,0],
  [13,0], [14,2], [15,3], [16,3], [17,7], [18,63]])
  assert.throws(() => decodeGuestDrawPacket(changed(word, value)));
for (let n = 0; n < packet().length; ++n)
  assert.throws(() => decodeGuestDrawPacket(packet().slice(0, n)), /truncated/);
assert.throws(() => decodeGuestDrawPacket(new Uint8Array([...packet(), 0])), /trailing/);
for (const min of [0,1]) for (const mag of [0,1]) for (const u of [0,1,2]) for (const v of [0,1,2]) {
  const bytes = packet(), view = new DataView(bytes.buffer);
  [min,mag,u,v].forEach((n,i) => view.setUint32((14+i)*4,n,true));
  const sampler = decodeGuestDrawPacket(bytes).fragmentTexture.sampler;
  assert.equal(sampler.minFilter, ['nearest','linear'][min]);
  assert.equal(sampler.magFilter, ['nearest','linear'][mag]);
  assert.equal(sampler.addressModeU, ['repeat','mirror-repeat','clamp-to-edge'][u]);
  assert.equal(sampler.addressModeV, ['repeat','mirror-repeat','clamp-to-edge'][v]);
}
console.log('GXM2 packet: 4x4 ABGR8, 32-byte guest / 16-byte packed rows, owned pixels, 36 samplers, rejection checks passed');
