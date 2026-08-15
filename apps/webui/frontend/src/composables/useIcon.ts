/**
 * Returns the URL for a named SVG icon from the icons directory.
 * Icons are served from the Vite publicDir (../assets), so they're
 * available at /icons/builtin/{name}.svg
 */
export function getIconUrl(name: string): string {
  return `/icons/builtin/${name}.svg`
}
