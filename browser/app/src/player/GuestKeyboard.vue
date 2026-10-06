<script setup lang="ts">
import { onMounted, ref } from 'vue';

// The game's on-screen keyboard (sceImeDialog): the device's own keyboard
// types into it.
const props = defineProps<{ text: string; maxLength: number; caret: number; enterLabel: string }>();
const emit = defineEmits<{ input: [text: string, caret: number]; enter: [text: string, caret: number]; close: [] }>();
const field = ref<HTMLInputElement | null>(null);
const value = ref(props.text);
onMounted(() => {
  field.value?.focus();
  field.value?.setSelectionRange(props.caret, props.caret);
});
const caret = () => field.value?.selectionStart ?? value.value.length;
</script>

<template>
  <form role="dialog" aria-modal="true" aria-label="Game keyboard" class="overlay-panel" @submit.prevent="emit('enter', value, caret())">
    <input
      ref="field"
      v-model="value"
      :maxlength="maxLength"
      class="field rounded-2xl"
      autocomplete="off"
      @input="emit('input', value, caret())"
      @keyup="($event.key.startsWith('Arrow')) && emit('input', value, caret())"
      @keydown.escape.prevent="emit('close')"
    >
    <div class="flex justify-end gap-2 mt-4">
      <button type="button" class="btn-text" @click="emit('close')">Cancel</button>
      <button type="submit" class="btn-filled">{{ enterLabel || 'Enter' }}</button>
    </div>
  </form>
</template>
