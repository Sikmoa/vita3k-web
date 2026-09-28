// Host controls share one pad state. Sources are independent: releasing a
// finger must not release the same button still held by a key/another finger.
export const SCE_CTRL = Object.freeze({ select: 0x1, start: 0x8, up: 0x10, right: 0x20,
  down: 0x40, left: 0x80, l: 0x100, r: 0x200, triangle: 0x1000, circle: 0x2000,
  cross: 0x4000, square: 0x8000 });
export const keyMap = Object.freeze({
  ArrowUp: { buttons: SCE_CTRL.up, axes: [0, -1, 0, 0] },
  ArrowDown: { buttons: SCE_CTRL.down, axes: [0, 1, 0, 0] },
  ArrowLeft: { buttons: SCE_CTRL.left, axes: [-1, 0, 0, 0] },
  ArrowRight: { buttons: SCE_CTRL.right, axes: [1, 0, 0, 0] },
  KeyX: { buttons: SCE_CTRL.cross }, KeyC: { buttons: SCE_CTRL.circle },
  KeyZ: { buttons: SCE_CTRL.square }, KeyV: { buttons: SCE_CTRL.triangle },
  KeyQ: { buttons: SCE_CTRL.l }, KeyE: { buttons: SCE_CTRL.r },
  Enter: { buttons: SCE_CTRL.start }, ShiftRight: { buttons: SCE_CTRL.select },
  KeyI: { axes: [0, 0, 0, -1] }, KeyK: { axes: [0, 0, 0, 1] },
  KeyJ: { axes: [0, 0, -1, 0] }, KeyL: { axes: [0, 0, 1, 0] },
});

export function createPadState(send) {
  const sources = new Map();
  let last = '';
  function flush(force = false) {
    let buttons = 0;
    const axes = [0, 0, 0, 0];
    for (const state of sources.values()) {
      buttons |= state.buttons || 0;
      state.axes?.forEach((value, index) => { axes[index] += value; });
    }
    const state = { buttons, axes: axes.map((n) => Math.max(-1, Math.min(1, n))) };
    const signature = JSON.stringify(state);
    if (force || signature !== last) { last = signature; send(state); }
  }
  return {
    set(id, state) { sources.set(id, state); flush(); },
    release(id) { if (sources.delete(id)) flush(); },
    clear() { sources.clear(); flush(true); },
    flush,
  };
}

// Pointer capture keeps a held control alive outside its bounds, and ensures
// release/cancel reaches us. One pointer owns a stick; buttons allow many.
export function createTouchControls(root, pad, { enabled = () => true, onGesture = () => {} } = {}) {
  const pointers = new Map();
  const activations = new Set();
  let activationId = 0;
  const source = (id) => `pointer:${id}`;
  function paint() {
    let buttons = 0;
    for (const p of pointers.values()) buttons |= p.state.buttons || 0;
    for (const button of root.querySelectorAll('[data-button], [data-direction]')) {
      button.classList.toggle('pressed', Boolean(buttons & SCE_CTRL[button.dataset.button || button.dataset.direction]));
    }
  }
  function update(event, pointer, initial = false) {
    const { element, kind } = pointer;
    const rect = element.getBoundingClientRect();
    const dx = event.clientX - rect.left - rect.width / 2;
    const dy = event.clientY - rect.top - rect.height / 2;
    if (kind === 'stick') {
      const radius = rect.width * .36;
      const distance = Math.hypot(dx, dy);
      const magnitude = Math.max(0, Math.min(1, (distance / radius - .12) / .88));
      const axes = [0, 0, 0, 0];
      const axis = Number(element.dataset.stick);
      axes[axis] = distance ? dx / distance * magnitude : 0;
      axes[axis + 1] = distance ? dy / distance * magnitude : 0;
      pointer.state = { axes };
      const travel = Math.min(distance, radius);
      element.firstElementChild.style.transform = `translate(${distance ? dx / distance * travel : 0}px, ${distance ? dy / distance * travel : 0}px)`;
      element.classList.add('active');
    } else if (kind === 'dpad') {
      let buttons = 0;
      const direction = initial && event.target.closest('[data-direction]')?.dataset.direction;
      if (direction) buttons = SCE_CTRL[direction];
      else if (Math.hypot(dx, dy) > rect.width * .12) {
        // Angular sectors overlap, so one thumb can press a diagonal.
        if (Math.abs(dx) > Math.abs(dy) * .45) buttons |= dx < 0 ? SCE_CTRL.left : SCE_CTRL.right;
        if (Math.abs(dy) > Math.abs(dx) * .45) buttons |= dy < 0 ? SCE_CTRL.up : SCE_CTRL.down;
      }
      pointer.state = { buttons, axes: [Number(Boolean(buttons & SCE_CTRL.right)) - Number(Boolean(buttons & SCE_CTRL.left)),
        Number(Boolean(buttons & SCE_CTRL.down)) - Number(Boolean(buttons & SCE_CTRL.up)), 0, 0] };
    } else pointer.state = { buttons: SCE_CTRL[element.dataset.button] };
    pad.set(source(event.pointerId), pointer.state);
    paint();
  }
  function release(id) {
    const pointer = pointers.get(id);
    if (!pointer) return;
    pointers.delete(id);
    pad.release(source(id));
    if (pointer.kind === 'stick') {
      pointer.element.classList.remove('active');
      pointer.element.firstElementChild.style.transform = '';
    }
    if (pointer.element.hasPointerCapture(id)) pointer.element.releasePointerCapture(id);
    paint();
  }
  root.addEventListener('pointerdown', (event) => {
    if (!enabled() || event.button !== 0) return;
    const element = event.target.closest('[data-stick], [data-dpad], [data-button]');
    if (!element) return;
    event.preventDefault();
    onGesture();
    const kind = element.hasAttribute('data-stick') ? 'stick' : element.hasAttribute('data-dpad') ? 'dpad' : 'button';
    if (kind === 'stick' && [...pointers.values()].some((p) => p.element === element)) return;
    const pointer = { element, kind, state: {} };
    pointers.set(event.pointerId, pointer);
    element.setPointerCapture(event.pointerId);
    update(event, pointer, true);
  });
  root.addEventListener('pointermove', (event) => {
    const pointer = pointers.get(event.pointerId);
    if (!pointer) return;
    event.preventDefault();
    update(event, pointer);
  });
  for (const type of ['pointerup', 'pointercancel', 'lostpointercapture'])
    root.addEventListener(type, (event) => release(event.pointerId));
  // Keyboard/assistive activation has no pointer events. Give it a short
  // press; ordinary touch/mouse clicks were already handled above.
  root.addEventListener('click', (event) => {
    if (event.detail !== 0 || !enabled()) return;
    const button = event.target.closest('[data-button], [data-direction]');
    if (!button) return;
    onGesture();
    const name = button.dataset.button || button.dataset.direction;
    const id = `activation:${++activationId}`;
    activations.add(id);
    pad.set(id, { buttons: SCE_CTRL[name], axes: [Number(name === 'right') - Number(name === 'left'),
      Number(name === 'down') - Number(name === 'up'), 0, 0] });
    setTimeout(() => { activations.delete(id); pad.release(id); }, 120);
  });
  root.addEventListener('contextmenu', (event) => event.preventDefault());
  return { clear() {
    for (const id of [...pointers.keys()]) release(id);
    for (const id of activations) pad.release(id);
    activations.clear();
  } };
}
