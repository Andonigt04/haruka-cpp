/** @type {import('tailwindcss').Config} */
// Tailwind theme for the Haruka Engine Doxygen docs, styled after the Haruka
// documentation/editor: dark blue-grey surfaces.
// Preflight is disabled so Tailwind does NOT reset Doxygen's own generated
// markup — utilities are used for the custom top navbar, while plain CSS in
// input.css restyles the Doxygen elements themselves.
module.exports = {
  corePlugins: { preflight: false },
  content: ['../header.html', '../footer.html'],
  darkMode: 'class',
  theme: {
    extend: {
      colors: {
        haruka: {
          bg:      '#1f232b', // page background (editor base)
          surface: '#282d38', // panels / navbar
          panel:   '#232833', // content panels, code blocks
          border:  '#3a4150',
          fg:      '#c7ccd6', // body text
          muted:   '#8b93a3',
          heading: '#ffffff',
          blue:     '#478cbf', // classic blue (accent)
          blueHi:   '#5fb0ef', // hover / bright accent
          link:     '#6cb5ff',
        },
      },
      fontFamily: {
        sans: ['Inter', 'Segoe UI', 'system-ui', 'sans-serif'],
        mono: ['JetBrains Mono', 'Fira Code', 'Consolas', 'monospace'],
      },
    },
  },
  plugins: [],
};
