// Real Chromium GLES2 -> WebGL pixel regression, with checked synthetic guest memory.
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL
  || new URL('../../build/playwright/node_modules/playwright/index.mjs', import.meta.url).href);
const source = await readFile(new URL('../web/gles_webgl.js', import.meta.url));
const server = createServer((req, res) => {
  res.setHeader('Content-Type', req.url === '/gles_webgl.js' ? 'text/javascript' : 'text/html');
  res.end(req.url === '/gles_webgl.js' ? source : '<!doctype html><title>GLES regression</title>');
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
let browser;
try {
  browser = await chromium.launch({ headless: true,
    args: ['--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--disable-dev-shm-usage'],
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
  });
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  const result = await page.evaluate(async () => {
    const { createGlesBridge } = await import('/gles_webgl.js');
    const heap = new Uint8Array(8 * 1024 * 1024), logs = [], frames = [];
    let top = 256, checks = 0;
    const check = (condition, message) => { ++checks; if (!condition) throw new Error(message); };
    const range = (p, n) => { if (!Number.isInteger(p) || p <= 0 || n < 0 || p + n > heap.length) throw new Error('guest range'); };
    const memory = {
      read(p, n) { range(p, n); return heap.slice(p, p + n); },
      write(p, data) { range(p, data.length); heap.set(data, p); },
      alloc(n) { const p = top; top = (top + n + 63) & ~63; range(p, n); return p; },
    };
    const put = value => {
      const data = typeof value === 'string' ? new TextEncoder().encode(value + '\0')
        : new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      const p = memory.alloc(data.length); memory.write(p, data); return p;
    };
    const ints = (...args) => put(new Int32Array(args));
    const int = p => new DataView(heap.buffer).getInt32(p, true);
    const text = p => new TextDecoder().decode(heap.subarray(p, heap.indexOf(0, p)));
    const bridge = createGlesBridge({ memory, logger: value => logs.push(value),
      onFrame: (generation, width, height, pixels) => frames.push({ generation, width, height, pixels }) });
    const call = (name, ...args) => bridge.call(name, args);
    check(!bridge.supports('glMapBufferOES'), 'unimplemented map buffer must not be advertised');
    check(!bridge.supports('glGetStringi'), 'GL3 entry point must not be advertised');
    const display = call('eglGetDisplay', 0), outputs = memory.alloc(256);
    check(display === 1 && call('eglInitialize', display, outputs, outputs + 4) === 1, 'EGL initialize');
    check(int(outputs) === 1 && int(outputs + 4) === 4, 'EGL version output');
    const configs = ints(0x3040, 4, 0x3024, 8, 0x3038);
    check(call('eglChooseConfig', display, configs, outputs, 1, outputs + 4) === 1 && int(outputs + 4) === 1, 'choose config');
    const surf = call('eglCreatePbufferSurface', display, 1, ints(0x3057, 8, 0x3056, 8, 0x3038));
    check(surf !== 0, 'pbuffer creation');
    check(call('eglCreateContext', display, 1, 0, ints(0x3098, 3, 0x3038)) === 0, 'ES3 must fail');
    check(call('eglGetError') === 0x3009, 'ES3 error must be EGL_BAD_MATCH');
    const ctx = call('eglCreateContext', display, 1, 0, ints(0x3098, 2, 0x3038));
    check(ctx !== 0 && call('eglMakeCurrent', display, surf, surf, ctx) === 1, 'current GLES2 context');
    check(text(call('glGetString', 0x1F02)).startsWith('OpenGL ES 2.0'), 'GLES version string');
    check(!text(call('glGetString', 0x1F03)).includes('mapbuffer'), 'mapbuffer extension hidden');
    call('glGetIntegerv', 0x0BA2, outputs);
    check(int(outputs + 8) === 8 && int(outputs + 12) === 8, 'viewport query writes four words');
    const shader = (type, source) => {
      const id = call('glCreateShader', type), ptr = put(source);
      call('glShaderSource', id, 1, ints(ptr), ints(source.length));
      call('glCompileShader', id); call('glGetShaderiv', id, 0x8B81, outputs);
      check(int(outputs) === 1, 'real shader compilation: ' + logs.join('\n')); return id;
    };
    const vertex = shader(0x8B31, 'attribute vec2 position; attribute vec2 uv; varying vec2 tex; uniform mat4 transform; void main(){ tex=uv; gl_Position=transform*vec4(position,0.,1.); }');
    const fragment = shader(0x8B30, 'precision mediump float; varying vec2 tex; uniform sampler2D image; uniform vec4 tint; void main(){ gl_FragColor=texture2D(image,tex)*tint; }');
    const program = call('glCreateProgram');
    call('glAttachShader', program, vertex); call('glAttachShader', program, fragment); call('glLinkProgram', program);
    call('glGetProgramiv', program, 0x8B82, outputs); check(int(outputs) === 1, 'real program linking');
    // Guest games compiled for the SGX driver rely on it not enforcing the
    // GLSL ES 1.00 cross-stage uniform precision match (ANGLE does). The
    // bridge must retry the link with the fragment default raised to highp.
    const mismatchVertex = shader(0x8B31, 'uniform highp float value; attribute vec4 position; varying vec2 tex; uniform mat4 transform; void main(){ tex=position.xy; gl_Position=transform*position; }');
    const mismatchFragment = shader(0x8B30, 'precision mediump float; varying vec2 tex; uniform mediump float value; uniform vec4 tint; void main(){ gl_FragColor=vec4(tint.rgb, value); }');
    const mismatch = call('glCreateProgram');
    call('glAttachShader', mismatch, mismatchVertex); call('glAttachShader', mismatch, mismatchFragment); call('glLinkProgram', mismatch);
    call('glGetProgramiv', mismatch, 0x8B82, outputs); check(int(outputs) === 1, 'cross-stage uniform precision mismatch is bridged (highp retry)');
    call('glUseProgram', program);
    const tint = call('glGetUniformLocation', program, put('tint'));
    call('glUniform4f', tint, 0.5, 1, 1, 1);
    call('glGetUniformfv', program, tint, outputs);
    check(new DataView(heap.buffer).getFloat32(outputs, true) === 0.5, 'scalar float uniform and query');
    check(call('glGetUniformLocation', program, put('absent')) === -1, 'inactive uniform returns -1');
    call('glUniform1f', 0xffffffff, 22); // legitimate no-op
    call('glUniform1i', call('glGetUniformLocation', program, put('image')), 0);
    call('glUniformMatrix4fv', call('glGetUniformLocation', program, put('transform')), 1, 0,
      put(new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1])));
    call('glGetActiveUniform', program, 0, 64, outputs, outputs + 4, outputs + 8, outputs + 16);
    check(int(outputs) > 0 && int(outputs + 4) === 1 && text(outputs + 16).length > 0, 'active uniform ABI');
    call('glGetShaderPrecisionFormat', 0x8B30, 0x8DF2, outputs, outputs + 8);
    check(int(outputs + 8) > 0, 'real shader float precision');
    const gen = name => { call(name, 1, outputs); return int(outputs); };
    const texture = gen('glGenTextures'); call('glBindTexture', 0x0DE1, texture);
    call('glTexParameteri', 0x0DE1, 0x2801, 0x2600); call('glTexParameteri', 0x0DE1, 0x2800, 0x2600);
    call('glTexImage2D', 0x0DE1, 0, 0x1908, 2, 2, 0, 0x1908, 0x1401,
      put(new Uint8Array([255,128,64,255, 255,128,64,255, 255,128,64,255, 255,128,64,255])));
    const target = gen('glGenTextures'); call('glBindTexture', 0x0DE1, target);
    call('glTexImage2D', 0x0DE1, 0, 0x1908, 8, 8, 0, 0x1908, 0x1401, 0);
    const fbo = gen('glGenFramebuffers'); call('glBindFramebuffer', 0x8D40, fbo);
    call('glFramebufferTexture2D', 0x8D40, 0x8CE0, 0x0DE1, target, 0);
    check(call('glCheckFramebufferStatus', 0x8D40) === 0x8CD5, 'complete real FBO');
    call('glBindTexture', 0x0DE1, texture);
    const buffer = gen('glGenBuffers'); call('glBindBuffer', 0x8892, buffer);
    const vertices = new Float32Array([-1,-1,0,0, 1,-1,1,0, 1,1,1,1, -1,1,0,1]);
    call('glBufferData', 0x8892, vertices.byteLength, put(vertices), 0x88E4);
    for (const [name, offset] of [['position', 0], ['uv', 8]]) {
      const loc = call('glGetAttribLocation', program, put(name));
      call('glEnableVertexAttribArray', loc); call('glVertexAttribPointer', loc, 2, 0x1406, 0, 16, offset);
    }
    const indices = gen('glGenBuffers'); call('glBindBuffer', 0x8893, indices);
    call('glBufferData', 0x8893, 12, put(new Uint16Array([0,1,2, 0,2,3])), 0x88E4);
    call('glClearColor', 0, 0, 0, 1); call('glClear', 0x4000); call('glDrawElements', 4, 6, 0x1403, 0);
    check(call('glGetError') === 0, 'no GL error during draw');
    const output = memory.alloc(256); call('glReadPixels', 0, 0, 8, 8, 0x1908, 0x1401, output);
    const center = Array.from(heap.slice(output + 4 * (4 * 8 + 4), output + 4 * (4 * 8 + 4) + 4));
    check(Math.abs(center[0] - 128) <= 1 && center[1] === 128 && center[2] === 64 && center[3] === 255,
      'textured FBO pixel: ' + center);
    // GLES permits client pointers whereas WebGL requires buffers. The bridge
    // must snapshot their bytes at draw time and preserve guest-visible state.
    const clientVertices = put(vertices), clientIndices = put(new Uint16Array([0,1,2, 0,2,3]));
    call('glBindBuffer', 0x8892, 0); call('glBindBuffer', 0x8893, 0);
    for (const [name, offset] of [['position', 0], ['uv', 8]]) {
      const loc = call('glGetAttribLocation', program, put(name));
      call('glVertexAttribPointer', loc, 2, 0x1406, 0, 16, clientVertices + offset);
      call('glGetVertexAttribPointerv', loc, 0x8645, outputs);
      check(int(outputs) === clientVertices + offset, 'client vertex pointer query');
    }
    call('glClear', 0x4000); call('glDrawElements', 4, 6, 0x1403, clientIndices);
    call('glReadPixels', 0, 0, 8, 8, 0x1908, 0x1401, output);
    check(Math.abs(heap[output + 4 * (4 * 8 + 4)] - 128) <= 1, 'client vertex/index arrays render');
    call('glGetIntegerv', 0x8894, outputs); call('glGetIntegerv', 0x8895, outputs + 4);
    check(int(outputs) === 0 && int(outputs + 4) === 0, 'streaming preserves guest buffer bindings');
    call('glBindBuffer', 0x8893, indices); call('glDrawElements', 4, 6, 0x1403, 0);
    check(call('glGetError') === 0, 'mixed client vertices and buffer indices');
    call('glBindFramebuffer', 0x8D40, 0); call('glClearColor', 0.25, 0.5, 0.75, 1); call('glClear', 0x4000);
    await bridge.swapBuffers();
    check(frames.length === 1 && frames[0].width === 8 && frames[0].pixels.length === 256, 'actual frame readback');
    check(Math.abs(frames[0].pixels[0] - 64) <= 1 && Math.abs(frames[0].pixels[2] - 191) <= 1, 'real presented pixel');
    call('glBindTexture', 0x0DE1, texture); call('glGetIntegerv', 0x8069, outputs);
    check(int(outputs) === texture, 'object query returns guest ID, not host object');
    call('glDeleteTextures', 1, ints(texture)); check(call('glIsTexture', texture) === 0, 'delete texture');
    check(call('eglMakeCurrent', display, 0, 0, 0) === 1, 'unbind context');
    check(call('eglDestroyContext', display, ctx) === 1 && call('eglDestroySurface', display, surf) === 1, 'destroy EGL handles');
    check(call('eglTerminate', display) === 1, 'terminate EGL');
    return { checks, center, logs };
  });
  assert.deepEqual(errors, []);
  console.log(`GLES WebGL Chromium: ${result.checks} checks passed; real textured pixel ${result.center}`);
} finally {
  await browser?.close();
  await new Promise(resolve => server.close(resolve));
}
