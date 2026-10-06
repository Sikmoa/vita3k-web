<script setup lang="ts">
import { computed, nextTick, ref, watch } from 'vue';
import { useClipboard } from '@vueuse/core';
import BaseDialog from '../components/BaseDialog.vue';

// The running game's log: live while open, kept scrolled to the end unless
// the reader scrolls up; copy or download it whole.
const props = defineProps<{ open: boolean; lines: string[]; version: number; title: string; savedTo: string | null }>();
const emit = defineEmits<{ close: [] }>();
// lines is the player's own array, not reactive: version says it grew.
const text = computed(() => (props.version, props.lines.join('\n')));
const box = ref<HTMLElement | null>(null);
const follow = ref(true);
const { copy, copied } = useClipboard({ legacy: true });

watch(() => [props.open, props.version], async () => {
  if (!props.open || !follow.value) return;
  await nextTick();
  if (box.value) box.value.scrollTop = box.value.scrollHeight;
});
const onScroll = () => {
  const el = box.value;
  if (el) follow.value = el.scrollTop + el.clientHeight >= el.scrollHeight - 24;
};
function download() {
  const link = document.createElement('a');
  link.href = URL.createObjectURL(new Blob([text.value + '\n'], { type: 'text/plain' }));
  link.download = `${props.title}-log.txt`;
  link.click();
  setTimeout(() => URL.revokeObjectURL(link.href), 10000);
}
</script>

<template>
  <BaseDialog :open="open" title="Logs" large @close="emit('close')">
    <pre
      ref="box"
      class="m-0 h-[60dvh] overflow-auto rounded-2xl bg-surface p-4 font-mono text-xs leading-relaxed text-on-surface-variant whitespace-pre-wrap break-words select-text"
      @scroll="onScroll"
    >{{ text || 'Nothing logged yet.' }}</pre>
    <div class="flex flex-wrap items-center gap-x-3 gap-y-1 mt-3 text-xs text-outline">
      <span>{{ lines.length }} lines{{ lines.length >= 5000 ? ' (the latest 5000)' : '' }}</span>
      <span v-if="savedTo">· saved to {{ savedTo }}</span>
      <span v-if="!follow">· scrolled up: new lines are not followed</span>
    </div>
    <template #actions>
      <button class="btn-text" @click="copy(text)"><span :class="copied ? 'i-lucide-check' : 'i-lucide-copy'" />{{ copied ? 'Copied' : 'Copy' }}</button>
      <button class="btn-tonal" @click="download"><span class="i-lucide-download" />Download</button>
    </template>
  </BaseDialog>
</template>
