<script setup lang="ts">
const model = defineModel<boolean>({ required: true });
defineProps<{ label: string; disabled?: boolean }>();
</script>

<template>
  <button
    type="button"
    role="switch"
    :aria-checked="model"
    :aria-label="label"
    :disabled="disabled"
    class="relative shrink-0 w-13 h-8 rounded-full border-0 cursor-pointer transition-colors duration-150 disabled:(opacity-60 cursor-not-allowed)"
    :class="model ? 'bg-primary' : 'bg-surface-highest'"
    @click="model = !model"
  >
    <span
      class="absolute top-1/2 -translate-y-1/2 rounded-full transition-all duration-150 flex items-center justify-center"
      :class="model ? 'left-[22px] w-6 h-6 bg-on-primary' : 'left-1.5 w-4 h-4 bg-outline'"
    >
      <span v-if="model" class="i-lucide-check text-[13px] text-primary" />
    </span>
  </button>
</template>
