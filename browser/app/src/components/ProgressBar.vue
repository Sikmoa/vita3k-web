<script setup lang="ts">
// value in [0, 1]; undefined shows an indeterminate bar.
defineProps<{ value?: number; label?: string }>();
</script>

<template>
  <div
    class="relative h-1.5 w-full overflow-hidden rounded-full bg-surface-highest"
    role="progressbar"
    :aria-label="label"
    :aria-valuenow="value === undefined ? undefined : Math.round(value * 100)"
    aria-valuemin="0"
    aria-valuemax="100"
  >
    <div
      v-if="value !== undefined"
      class="h-full rounded-full bg-primary transition-[width] duration-200"
      :style="{ width: `${Math.round(Math.min(1, Math.max(0, value)) * 100)}%` }"
    />
    <div v-else class="indeterminate absolute inset-y-0 w-1/3 rounded-full bg-primary" />
  </div>
</template>

<style scoped>
.indeterminate { animation: slide 1.2s ease-in-out infinite; }
@keyframes slide { from { left: -33%; } to { left: 100%; } }
</style>
