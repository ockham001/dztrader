import { ref } from 'vue'

export type ThemeName = 'light' | 'dark' | 'system'

const THEME_KEY = 'theme'
const currentTheme = ref<ThemeName>(loadTheme())

function loadTheme(): ThemeName {
  try {
    const v = localStorage.getItem(THEME_KEY)
    if (v === 'light' || v === 'dark' || v === 'system') return v
  } catch { /* ignore */ }
  return 'system'
}

function applyThemeClass(theme: ThemeName): void {
  const root = document.documentElement
  root.classList.remove('light', 'dark')
  if (theme === 'light') root.classList.add('light')
  else if (theme === 'dark') root.classList.add('dark')
  // 'system' → no class, @media handles it
}

export function useTheme() {
  function setTheme(theme: ThemeName): void {
    currentTheme.value = theme
    try { localStorage.setItem(THEME_KEY, theme) } catch { /* ignore */ }
    applyThemeClass(theme)
  }

  // Apply on init
  applyThemeClass(currentTheme.value)

  return { currentTheme, setTheme }
}
