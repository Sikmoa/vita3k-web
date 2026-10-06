<script setup lang="ts" generic="T extends string | number">
const model = defineModel<T>({ required: true });
defineProps<{ options: { value: T; label: string; icon?: string }[]; label: string; disabled?: boolean }>();
</script>

<template>
  <div class="inline-flex p-1 gap-1 rounded-full bg-surface-highest" role="radiogroup" :aria-label="label">
    <button
      v-for="option in options"
      :key="String(option.value)"
      type="button"
      role="radio"
      :aria-checked="model === option.value"
      :disabled="disabled"
      class="inline-flex items-center gap-1.5 h-8 px-4 rounded-full border-0 text-sm font-medium cursor-pointer transition-colors duration-150 disabled:(opacity-40 cursor-not-allowed)"
      :class="model === option.value ? 'bg-primary text-on-primary' : 'bg-transparent text-on-surface-variant hover:text-on-surface'"
      @click="model = option.value"
    >
      <span v-if="option.icon" :class="option.icon" />
      {{ option.label }}
    </button>
  </div>
</template>
