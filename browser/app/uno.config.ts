import { defineConfig } from 'unocss';
import presetWind3 from '@unocss/preset-wind3';
import presetIcons from '@unocss/preset-icons';

// The palette (dark scheme): surfaces from lowest to highest, primary and
// secondary with their containers, and the error roles.
const colors = {
  'primary': '#AFDAFB',
  'on-primary': '#05344B',
  'primary-container': '#094C6B',
  'on-primary-container': '#E5F3FF',
  'secondary': '#C2D7EA',
  'secondary-container': '#354958',
  'on-secondary-container': '#E4F3FF',
  'tertiary-container': '#50415B',
  'on-tertiary-container': '#F8EDFE',
  'surface': '#0F1417',
  'surface-low': '#181C1F',
  'surface-container': '#1C2023',
  'surface-high': '#262A2E',
  'surface-highest': '#313539',
  'on-surface': '#DEE3E8',
  'on-surface-variant': '#CAD6E1',
  'outline': '#A1ADB7',
  'outline-variant': '#6D7882',
  'inverse-surface': '#DEE3E8',
  'inverse-on-surface': '#2D3134',
  'inverse-primary': '#1E6489',
  'error': '#F2B8B5',
  'on-error': '#601410',
  'error-container': '#8C1D18',
  'on-error-container': '#F9DEDC',
};

export default defineConfig({
  presets: [
    presetWind3(),
    presetIcons({ scale: 1.2, extraProperties: { 'display': 'inline-block', 'vertical-align': 'middle', 'flex-shrink': '0' } }),
  ],
  theme: {
    colors,
    borderRadius: { card: '20px', dialog: '28px' },
    fontFamily: { sans: 'Inter, system-ui, -apple-system, "Segoe UI", Roboto, sans-serif', mono: 'ui-monospace, SFMono-Regular, Menlo, monospace' },
  },
  shortcuts: {
    'btn': 'inline-flex items-center justify-center gap-2 h-10 px-5 rounded-full border-0 font-medium text-sm cursor-pointer select-none transition-colors duration-150 disabled:(opacity-40 cursor-not-allowed)',
    'btn-filled': 'btn bg-primary text-on-primary hover:bg-[#c3e4fc]',
    'btn-tonal': 'btn bg-secondary-container text-on-secondary-container hover:bg-[#40566a]',
    'btn-text': 'btn bg-transparent text-primary hover:bg-[#afdafb14] px-3',
    'btn-danger': 'btn bg-error-container text-on-error-container hover:bg-[#a0241e]',
    'icon-btn': 'inline-flex items-center justify-center w-10 h-10 rounded-full border-0 bg-transparent text-on-surface-variant cursor-pointer hover:bg-[#dee3e814] transition-colors duration-150 disabled:(opacity-40 cursor-not-allowed)',
    'card': 'bg-surface-container rounded-card',
    'field': 'h-12 w-full rounded-full border-0 bg-surface-high text-on-surface px-5 text-sm outline-none focus:(ring-2 ring-primary) placeholder:text-outline-variant',
    'chip': 'inline-flex items-center gap-1 h-6 px-2.5 rounded-full text-xs font-medium',
    'overlay-panel': 'absolute z-4 left-1/2 top-1/2 -translate-x-1/2 -translate-y-1/2 w-[min(480px,calc(100%-32px))] max-h-[calc(100%-32px)] overflow-auto p-6 rounded-dialog bg-surface-high text-on-surface',
    'label-text': 'text-xs font-medium tracking-wide text-on-surface-variant',
  },
});
