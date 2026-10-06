<script setup lang="ts">
import { onMounted } from 'vue';
import { route } from './router';
import { refreshLibrary } from './library';
import AppNav from './components/AppNav.vue';
import LibraryPage from './pages/LibraryPage.vue';
import SettingsPage from './pages/SettingsPage.vue';
import AboutPage from './pages/AboutPage.vue';
import FilesPage from './pages/FilesPage.vue';
import PlayPage from './pages/PlayPage.vue';

onMounted(refreshLibrary);
</script>

<template>
  <!-- A game takes the whole window; the other pages share the navigation. -->
  <PlayPage v-if="route.page === 'play'" :key="route.title + (route.source ?? '')" :title="route.title" :source="route.source" />
  <div v-else class="lg:flex min-h-full">
    <AppNav />
    <main class="flex-1 min-w-0">
      <LibraryPage v-if="route.page === 'library'" />
      <SettingsPage v-else-if="route.page === 'settings'" />
      <FilesPage v-else-if="route.page === 'files'" :path="route.path" />
      <AboutPage v-else />
    </main>
  </div>
</template>
