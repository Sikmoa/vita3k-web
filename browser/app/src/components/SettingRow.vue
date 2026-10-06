<script setup lang="ts">
// One setting: what it is and does on the left, its control on the right
// (below on narrow screens when `stack`).
defineProps<{ title: string; description?: string; stack?: boolean }>();
</script>

<template>
  <div class="flex gap-4 px-5 py-4" :class="stack ? 'flex-col sm:flex-row sm:items-center' : 'items-center'">
    <div class="flex-1 min-w-0">
      <div class="text-base text-on-surface">{{ title }}</div>
      <div v-if="description || $slots.description" class="text-sm text-on-surface-variant mt-0.5 leading-relaxed"><slot name="description">{{ description }}</slot></div>
    </div>
    <div class="shrink-0"><slot /></div>
  </div>
</template>
