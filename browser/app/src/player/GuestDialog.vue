<script setup lang="ts">
import { nextTick, ref, watch } from 'vue';

// The game's own message dialog (sceMsgDialog, save prompts): its buttons
// answer it; a pad's D-pad moves the selection (PlayPage).
const props = defineProps<{ message: string; buttons: string[]; selected: number; progress: number | null; hint: string }>();
const emit = defineEmits<{ press: [index: number] }>();
const root = ref<HTMLElement | null>(null);
watch(() => props.selected, async () => {
  await nextTick();
  root.value?.querySelector<HTMLElement>('[data-selected="true"]')?.focus({ preventScroll: true });
}, { immediate: true });
</script>

<template>
  <div ref="root" role="dialog" aria-modal="true" :aria-label="message" class="overlay-panel text-center">
    <p class="m-0 mb-5 whitespace-pre-wrap break-words leading-relaxed">{{ message }}</p>
    <div v-if="progress !== null" class="h-1.5 rounded-full bg-surface-highest overflow-hidden mb-5">
      <div class="h-full bg-primary" :style="{ width: `${progress}%` }" />
    </div>
    <div class="flex flex-wrap justify-center gap-2">
      <button
        v-for="(label, index) in buttons"
        :key="index"
        :data-selected="index === selected"
        :class="index === selected ? 'btn-filled' : 'btn-tonal'"
        @click="emit('press', index)"
      >
        {{ label }}
      </button>
    </div>
    <small v-if="buttons.length" class="block mt-4 text-xs text-outline">{{ hint }}</small>
  </div>
</template>
