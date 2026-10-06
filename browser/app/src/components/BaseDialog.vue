<script setup lang="ts">
import { nextTick, ref, watch } from 'vue';
import { onKeyStroke } from '@vueuse/core';

// A modal dialog: a scrim, then the panel (28 px corners); Escape and the
// scrim close it unless `locked` (work in progress).
const props = defineProps<{ open: boolean; title: string; locked?: boolean; wide?: boolean; large?: boolean }>();
const emit = defineEmits<{ close: [] }>();
const panel = ref<HTMLElement | null>(null);

const close = () => { if (!props.locked) emit('close'); };
onKeyStroke('Escape', (event) => { if (props.open) { event.preventDefault(); close(); } });
watch(() => props.open, async (open) => {
  if (!open) return;
  await nextTick();
  panel.value?.querySelector<HTMLElement>('[autofocus], button, input')?.focus();
});
</script>

<template>
  <Teleport to="body">
    <Transition name="dialog">
      <div v-if="open" class="fixed inset-0 z-50 flex items-end sm:items-center justify-center bg-[#000000a6] sm:p-6" @click.self="close">
        <section
          ref="panel"
          role="dialog"
          aria-modal="true"
          :aria-label="title"
          class="w-full max-h-[92dvh] overflow-auto bg-surface-high text-on-surface rounded-t-dialog sm:rounded-dialog p-6 pb-[max(24px,env(safe-area-inset-bottom))]"
          :class="large ? 'sm:max-w-4xl' : wide ? 'sm:max-w-xl' : 'sm:max-w-md'"
        >
          <header class="flex items-start gap-3 mb-4">
            <h2 class="flex-1 m-0 text-2xl font-normal">{{ title }}</h2>
            <button class="icon-btn -mr-2 -mt-1" aria-label="Close dialog" :disabled="locked" @click="close">
              <span class="i-lucide-x text-xl" />
            </button>
          </header>
          <slot />
          <footer v-if="$slots.actions" class="flex flex-wrap justify-end gap-2 mt-6">
            <slot name="actions" />
          </footer>
        </section>
      </div>
    </Transition>
  </Teleport>
</template>

<style scoped>
.dialog-enter-active, .dialog-leave-active { transition: opacity 0.18s ease; }
.dialog-enter-active section, .dialog-leave-active section { transition: transform 0.18s ease; }
.dialog-enter-from, .dialog-leave-to { opacity: 0; }
.dialog-enter-from section, .dialog-leave-to section { transform: translateY(16px); }
</style>
