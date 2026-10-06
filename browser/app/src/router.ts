import { computed, ref } from 'vue';

// Hash routes, so the site works from any static path without server
// rewrites: #/library (default), #/settings, #/about, #/files[/<folder>/…],
// #/play/<title>[?source=package].
export type Page = 'library' | 'settings' | 'about' | 'files' | 'play';
export interface Route { page: Page; title: string; source: string | null; path: string[] }

function parse(hash: string): Route {
  const [location, query = ''] = hash.replace(/^#\/?/, '').split('?');
  const [page, ...rest] = location.split('/');
  const none = { title: '', source: null, path: [] };
  if (page === 'settings' || page === 'about') return { page, ...none };
  if (page === 'files') return { page, ...none, path: rest.filter(Boolean).map(decodeURIComponent) };
  if (page === 'play' && rest[0]) return { page, ...none, title: decodeURIComponent(rest[0]), source: new URLSearchParams(query).get('source') };
  return { page: 'library', ...none };
}

const hash = ref(location.hash);
addEventListener('hashchange', () => { hash.value = location.hash; });

export const route = computed(() => parse(hash.value));

export function href(page: Page, title = '', source: string | null = null) {
  if (page === 'play') return `#/play/${encodeURIComponent(title)}${source ? `?source=${source}` : ''}`;
  return `#/${page}`;
}
export function filesHref(path: string[]) {
  return ['#/files', ...path.map(encodeURIComponent)].join('/');
}
export function navigate(page: Page, title = '', source: string | null = null) {
  location.hash = href(page, title, source);
}
