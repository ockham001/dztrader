---
name: DZTrader
description: Quant trading operations console — dense, quiet, status-first
colors:
  ink-strongest: "#171717"
  ink-secondary: "#404040"
  ink-tertiary: "#737373"
  ink-disabled: "#A1A1A1"
  paper-base: "#FFFFFF"
  paper-secondary: "#F5F5F5"
  paper-tertiary: "#E5E5E5"
  ink-strongest-dark: "#E5E5E5"
  ink-secondary-dark: "#A1A1A1"
  paper-base-dark: "#171717"
  paper-secondary-dark: "#262626"
  paper-tertiary-dark: "#404040"
  brand-violet: "#4B3FE3"
  brand-violet-hover: "#6A6FFF"
  brand-violet-active: "#3F31C6"
  invert-light: "#262626"
  invert-dark: "#E5E5E5"
  overlay-l1: "rgba(115, 115, 115, 0.08)"
  overlay-l2: "rgba(115, 115, 115, 0.12)"
  overlay-l1-dark: "#d4d4d40a"
  overlay-l2-dark: "#d4d4d40f"
  on-invert-light: "#FFFFFF"
  on-invert-dark: "#171717"
  surface-card-light: "#F5F5F5"
  surface-card-dark: "#262626"
  surface-panel-light: "#F5F5F5"
  surface-panel-dark: "#202020"
  rise-crimson: "#D4543E"
  fall-jade: "#2A9D5C"
  status-success: "#15A877"
  status-alert: "#FEA900"
  status-warning: "#E27900"
  status-error: "#E8463A"
  status-primary: "#2F74FF"
  contract-gold: "#B8860B"
  contract-gold-dark: "#D4A017"
typography:
  display:
    fontFamily: "\"SF Pro\", \"PingFang SC\", system-ui, -apple-system, \"Segoe UI\", Roboto, sans-serif"
    fontSize: "52px"
    fontWeight: 600
    lineHeight: "60px"
    letterSpacing: "-0.03em"
  headline-md:
    fontFamily: "\"SF Pro\", \"PingFang SC\", system-ui, -apple-system, \"Segoe UI\", Roboto, sans-serif"
    fontSize: "22px"
    fontWeight: 600
    lineHeight: "32px"
  title-sm:
    fontFamily: "\"SF Pro Text\", \"PingFang SC\", system-ui, -apple-system, \"Segoe UI\", Roboto, sans-serif"
    fontSize: "17px"
    fontWeight: 600
    lineHeight: "26px"
  body-base:
    fontFamily: "\"SF Pro Text\", \"PingFang SC\", system-ui, -apple-system, \"Segoe UI\", Roboto, sans-serif"
    fontSize: "15px"
    fontWeight: 400
    lineHeight: "22px"
    letterSpacing: "-0.02em"
  body-sm:
    fontFamily: "\"SF Pro Text\", \"PingFang SC\", system-ui, -apple-system, \"Segoe UI\", Roboto, sans-serif"
    fontSize: "12px"
    fontWeight: 400
    lineHeight: "18px"
  body-xs:
    fontFamily: "\"SF Pro Text\", \"PingFang SC\", system-ui, -apple-system, \"Segoe UI\", Roboto, sans-serif"
    fontSize: "11px"
    fontWeight: 400
    lineHeight: "16px"
  label-md:
    fontFamily: "\"SF Pro Text\", \"PingFang SC\", system-ui, -apple-system, \"Segoe UI\", Roboto, sans-serif"
    fontSize: "13px"
    fontWeight: 500
    lineHeight: "20px"
  metric:
    fontFamily: "\"Inter\", \"SF Pro Text\", \"PingFang SC\", system-ui, -apple-system, \"Segoe UI\", Roboto, sans-serif"
    fontSize: "11px"
    fontWeight: 400
    lineHeight: "16px"
  code:
    fontFamily: "\"JetBrains Mono\", ui-monospace, \"SF Mono\", Menlo, Consolas, monospace"
    fontSize: "13px"
    fontWeight: 450
    lineHeight: "20px"
rounded:
  "0": "0px"
  "2": "2px"
  "4": "4px"
  "6": "6px"
  "8": "8px"
  "10": "10px"
  "12": "12px"
  "16": "16px"
  "20": "20px"
  "24": "24px"
  "32": "32px"
  full: "999px"
spacing:
  "0": "0px"
  "2": "2px"
  "3": "3px"
  "4": "4px"
  "6": "6px"
  "8": "10px"
  "10": "12px"
  "12": "14px"
  "16": "18px"
  "20": "24px"
  "24": "28px"
  "32": "36px"
  "40": "44px"
  "48": "48px"
  "64": "64px"
components:
  button-primary:
    backgroundColor: "{colors.invert-light}"
    textColor: "{colors.on-invert-light}"
    rounded: "{rounded.8}"
    padding: "0 14px"
    height: "28px"
  button-secondary:
    backgroundColor: "{colors.overlay-l1}"
    textColor: "{colors.ink-strongest}"
    rounded: "{rounded.8}"
    padding: "0 14px"
    height: "28px"
  button-brand:
    backgroundColor: "{colors.brand-violet}"
    textColor: "{colors.paper-base}"
    rounded: "{rounded.8}"
    padding: "0 14px"
    height: "28px"
  button-danger:
    backgroundColor: "{colors.status-error}"
    textColor: "{colors.paper-base}"
    rounded: "{rounded.8}"
    padding: "0 14px"
    height: "28px"
  input:
    backgroundColor: "{colors.overlay-l1}"
    textColor: "{colors.ink-strongest}"
    rounded: "{rounded.8}"
    padding: "0 14px"
    height: "32px"
  card:
    backgroundColor: "{colors.surface-card-light}"
    textColor: "{colors.ink-strongest}"
    rounded: "{rounded.12}"
    padding: "24px"
  panel:
    backgroundColor: "{colors.surface-panel-light}"
    textColor: "{colors.ink-strongest}"
    rounded: "{rounded.12}"
    padding: "0"
  tag:
    backgroundColor: "{colors.overlay-l2}"
    textColor: "{colors.ink-secondary}"
    rounded: "{rounded.8}"
    padding: "0 10px"
    height: "22px"
---

# Design System: DZTrader

## Overview

**Creative North Star: "The Instrument Panel"**

This is a system built to be *glanced at*, not read. Every screen answers one question — is everything still running — and the visual language is engineered for that answer to arrive in under a second, at a distance, while the operator is also watching something else.

The register is deliberately generic — neutral system UI, no brand gesture, no signature flourish. This is a tool surface: it should get out of the way and be instantly familiar to anyone who has used an admin console. There is no custom typeface, no expressive color, no illustrative treatment. The design's contribution is restraint and consistency, not personality. Density is a feature here, not a compromise: the system earns trust by showing a lot at once and letting the exceptions find the eye, never by spreading three numbers across three screens.

Depth is nearly absent. Panels sit flat on the page separated by hairlines; elevation is reserved for things that float above the document (menus, dialogs, tooltips) and therefore need to be distinguishable from it. Typography carries hierarchy instead of size jumps and drop shadows: a 13px semibold and a 15px regular read as clearly different ranks when everything around them is 11–12px.

Color is spent carefully and only on state. The chrome is achromatic on purpose so that the chromatic families that do appear — brand violet, rise/fall, and the five status hues — each mean exactly one thing. When the whole interface is grey, a single amber cell is unmissable.

**Key Characteristics:**
- Neutral system UI stack; no brand typeface, no expressive chrome
- Density over breathing room; 11–15px body range, compact 32px control heights
- Achromatic chrome; color reserved for state, never decoration
- Flat panels, hairline separation, elevation only for overlay
- Light and dark are peers, not modes — both are first-class, fully specified
- Numerals get tabular figures and monospace faces; text never does

## Colors

A near-achromatic grey spine, with three chromatic families allowed to speak: brand violet (identity), rise/fall (direction), and the five status hues (process state). A fourth family, contract gold, exists in the token file but has no consumers — see below.

### Primary

- **Brand Violet** (#4B3FE3): Identity only. The brand mark, the login submit button, the solid `admin` role badge. It is *not* the color of ordinary buttons, switches, or tags — those use the ink scale. This restriction is what keeps violet legible as a signal rather than as wallpaper.

### Secondary

- **Rise Crimson** (#D4543E) and **Fall Jade** (#2A9D5C): Direction of change. **Red is up, green is down** — Chinese futures convention, the inverse of the Western default. This is a domain invariant, not a palette preference. Applied to position deltas, trade side, and P&L; never to decoration.
- **Status hues** (five families) — Success `#15A877`, Alert `#FEA900`, Warning `#E27900`, Error `#E8463A`, Primary `#2F74FF`: Process and connection state. Each ships a three-step surface ramp (`-surface-l1/l2/l3`) for tag and badge backgrounds, plus a separate `-on-surface` text step that darkens in light mode and lightens in dark mode so the label clears 4.5:1 on top of its own tinted ground. Note the ramps are not uniform: primary and success step 12/18/36% alpha, warning and error 12/16/36%, alert 14/20/36%.
- **Contract Gold** (`#B8860B` light / `#D4A017` dark): **Unused.** The token pair `--bg-contract-main` / `--text-on-contract-main` is defined with the stated intent of a main-contract badge, but no component consumes it and no such badge exists in the codebase. Recorded here so it is not mistaken for a live semantic color.

### Neutral

- **Ink Scale** — Strongest `#171717`, Secondary `#404040`, Tertiary `#737373`, Disabled `#A1A1A1` (light) and the inverse‑weighted set `#E5E5E5` / `#A1A1A1` / `#737373` / `#525252` (dark).
- **Paper Scale** — Base `#FFFFFF`, Secondary `#F5F5F5`, Tertiary `#E5E5E5` (light); Base `#171717`, Secondary `#262626`, Tertiary `#404040` (dark).
- **Overlay steps** — Four alpha levels (`l1` 8% → `l4` 20%) of a mid-grey, used for hover grounds, hover states, scrims, and input fills. They compose over any background, so one token works on both paper levels.

### Named Rules

**The One Job Rule.** Every chromatic family has exactly one meaning. Violet is identity, crimson/jade is direction, the five status hues are process state. Gold is reserved for main-contract instruments but is currently unassigned — no component consumes it. Nothing decorative is ever allowed to borrow a family — the moment amber can mean "attention" *or* "accent", the amber cell stops working.

**The Achromatic Chrome Rule.** Backgrounds, borders, text, icons, and every ordinary control are grey. If a surface needs to feel important, it gets weight or position — not hue.

**The Paired-Step Rule.** Every status color exists in three coordinated forms: a solid `-default` (dots, bars, icons), a transparent `-surface` (tag grounds), and a text `-on-surface` (tag labels). Use them as a set. Substituting one for another is what produced the contrast failures this system already had to repair.

## Typography

**Display Font:** SF Pro / PingFang SC (system stack)
**Body Font:** SF Pro Text / PingFang SC (system stack)
**Label/Metric Font:** Inter (numerals and counts)
**Mono Font:** JetBrains Mono (codes, IDs, prices)

**Character:** A system UI stack that disappears. There is no brand typeface and no editorial gesture — the type is Apple-native in rhythm, tight-tracked at large sizes, and completely neutral at reading sizes. The only typographic expressiveness lives in the metric and mono faces, where alignment matters more than personality.

### Hierarchy

- **Display** (600, 52px/60px, -0.03em): Defined in the scale but **unused** — no screen currently consumes it. The largest type in the running app is 48px, in the About dialog's product name.
- **Headline** (600, 22–32px, 32–40px): Login page title (26px) and page-level headings. The app shell topbar title is an exception at 12px semibold.
- **Title** (600, 14–17px/22–26px): Panel titles, card titles, dialog titles.
- **Body** (400, 15px/22px, -0.02em): Default reading size for dialogs, forms, and table cells.
- **Small body** (400, 12px/18px): Panel sub-labels, meta rows, compact tags.
- **Micro** (400, 11px/16px): Topbar chrome, timestamps, status badges, count pills.
- **Label** (500, 13px/20px): Form labels, table headers (uppercase), button text at body scale.

### Named Rules

**The Tabular Numerals Rule.** Any figure that can change — price, quantity, P&L, count, percentage — carries `font-variant-numeric: tabular-nums` and usually a mono or metric face. Text never switches faces to change weight; numbers never align to proportional widths. Without this, a row of updating prices jitters and the whole panel reads as broken.

**The No Display Type In The Shell Rule.** The 40–52px display steps are dead tokens today: nothing consumes them. The largest type actually rendered anywhere is the 48px About-dialog product name; the login title stops at 26px. Inside the app shell, treat 32px as the practical ceiling and keep the interface between 11 and 17px. Enlarging a number to make it important destroys the density the panel depends on.

## Layout

The shell is a two-column grid: a sticky 220px sidebar (collapsing to a 56px icon rail) beside a workspace column. Below 768px the sidebar leaves the flow entirely and becomes an off-canvas drawer. The topbar is a 32px sticky strip — deliberately thin, because it is permanent chrome and every pixel it takes is a pixel the panels cannot use.

Breakpoints are Tailwind-aligned and closed: `base <640`, `sm ≥640`, `md ≥768`, `lg ≥1024`, `xl ≥1280`. The width values are whitelisted and machine-enforced by a test, so a new arbitrary breakpoint is a build failure rather than a slow drift. Layout reflow lives in CSS media queries; behavioral branches (drawer versus rail) live in a `matchMedia` composable, so the two never disagree about the same width.

The dashboard is a three-zone grid — left column, a 6px drag gutter, right column — with a bottom band. Panels are resizable via injected flex coefficients; the track sizing stays in CSS so media queries can still override it without `!important`. Panel gutters are 6px; internal panel padding is drawn from {0, 1, 4, 6, 10, 14, 18}px, with region headers and footers at 10px/18px and table cells at 6px/14px. The whole point is that the full status set fits on one laptop or tablet screen without scrolling.

Spacing is scale-free by design: the token names are pixel values (`--spacer-8` is 10px, `--spacer-16` is 18px), not t-shirt sizes. This looks wrong and is intentional — the values were inherited from a design tool export and renaming them would have silently re-bound hundreds of call sites.

## Elevation & Depth

Overwhelmingly flat. Panels are separated from the page by a 1px hairline and by a tonal step in the paper scale, not by shadow. A dashboard panel does carry one nearly-invisible shadow (`0 1px 3px` at 6% ink) that reads as a crisp edge more than as lift.

Shadows appear only where something genuinely floats: menus, dropdowns, popovers, dialogs, drawers. The floating-layer shadows are built with `color-mix()` against the current text color rather than hardcoded black, so one definition produces a soft grey in light mode and a deep, correct shadow in dark mode without a second token. Note this is not uniform: the three `--shadow-sm/lg/xl` utility tokens in `design-tokens.css` are still hardcoded `rgb(0 0 0 / …)` and have eight call sites outside their own definition — new floating layers should follow the `color-mix()` form, not those.

Backdrop blur is used twice, both defensibly: the topbar blurs content scrolling beneath it (with a gradient fade at its lower edge so text dissolves rather than clipping), and the modal overlay blurs the page behind it.

### Shadow Vocabulary

- **Edge** (`0 1px 3px` ink @6%): Dashboard panels. Barely a shadow; provides the panel boundary.
- **Popover** (`0 12px 32px` ink @16%, `0 2px 8px` ink @8%): Dropdowns, menus, tooltips.
- **Dialog** (`0 24px 64px` ink @14%, `0 4px 16px` ink @8%): Modals and drawers.

### Named Rules

**The Elevation-Is-A-Promise Rule.** A shadow means the thing is above the document and can be dismissed. Nothing that stays put gets one. Panels are not cards floating over a background; they are the background.

**The Shift-Not-Scale Rule.** Interactive feedback is a background-alpha change or a border-color change at 120ms. Nothing grows, lifts, or bounces on hover. In a surface watched for hours, movement is a cost.

## Shapes

Corners are soft but restrained: the workhorse radius is 8px on every control (buttons, inputs, selects, tags, nav items), 12px on every container (cards, panels, dialogs, drawers, empty states). Larger radii exist in the scale (16/20/24/32) but are essentially reserved; `--radius-full` is for dots, pills, and the count badges on tabs.

Borders are 1px and almost always `--border-neutral-l1` — a 12% grey that is visible on paper-secondary but nearly invisible on paper-base. Focused inputs thicken to 2px and shift to `--border-neutral-l3` (36%), which is the system's one border-weight change.

The `1px → 2px` focus and the 8/12 pairing are the entire form language. There is no clipping, no cut corners, no asymmetric geometry, no decorative border.

### Named Rules

**The 8-And-12 Rule.** Controls are 8px, containers are 12px. A control inside a container aligns its radius to the control scale, never to the container's. This one decision is why the whole interface looks like one system even though it spans a hundred components.

**The Hairline-Only Rule.** Separation is a 1px `--border-neutral-l1` rule or a tonal step — never both at once, never a heavier divider to signal emphasis. Emphasis comes from type weight and position.

## Components

### Buttons

- **Shape:** 8px radius, three fixed heights — 24px (`--sm`), 28px (`--md`, the default), 32px (`--lg`). Horizontal padding 10/14/18px by size. Never sized by content.
- **Primary:** Inverted ink — dark fill on light mode, light fill on dark mode. This is the confirmation action. Notably *not* brand violet.
- **Secondary:** Overlay-l1 ground with a hairline border; hover steps to l2.
- **Tertiary / Ghost:** Transparent until hover, then overlay-l2.
- **Brand:** Violet fill, used once per screen at most (the login submit).
- **Danger / Danger Subtle / Warning:** Status-error and status-warning fills, or their `-surface` tint for the subtle variant.
- **Hover / Focus:** Background and border-color transitions at 120ms. No transform, no shadow growth.
- **Disabled:** Primary swaps to `--text-oninvert-disabled`, a dedicated token that exists specifically because the generic disabled grey is the same value as the disabled ground in dark mode and produced invisible text.

### Chips / Tags

- **Style:** 22px tall, 8px radius, overlay-l2 ground with a hairline border, 12px label.
- **State:** The tinted variants (`--success`, `--warning`, `--danger`, `--brand`) swap the ground to a status `-surface` ramp and the label to the matching `-on-surface` step. The neutral variant uses a white overlay rather than grey in both themes.

### Cards / Panels

- **Corner Style:** 12px.
- **Background:** `--bg-base-secondary` for cards; dashboard panels use `--bg-card`, which differs from it (light `#F5F5F5` vs `#F5F5F5` — same; dark `#202020` vs `#262626` — panels are *darker* than cards).
- **Shadow Strategy:** See Elevation — edge shadow on panels, none on static cards.
- **Border:** 1px hairline.
- **Internal Padding:** 24px for cards. Panels draw per-region padding from {0, 1, 4, 6, 10, 14, 18}px; the header row is `padding: var(--spacer-8) var(--spacer-16)` — which resolves to **10px / 18px**, not 8/16 — followed by a hairline separator.

### Dialogs / Drawers

- **Shape:** 12px radius, max-width 520px (form variant 560px) or 360px for the drawer; `max-height: calc(100vh - 48px)` with a scrolling body so the header and footer stay pinned.
- **Background:** `--bg-base-secondary` for dialogs, `--bg-base-default` for drawers — the drawer sits a step lighter than a dialog on purpose.
- **Elevation:** the dialog shadow step (24px/64px ink at 14%, plus a 4px/16px contact shadow).
- **Structure:** header and footer are separated by spacing only — neither gets a `border-bottom`/`border-top`, which is why the dialog reads as one surface rather than three stacked bands.
- **Scrim:** `.modal-overlay` is a `color-mix()` tint of the page ground at 60% plus `backdrop-filter: blur(4px)`, so the page dissolves rather than simply dimming.

### Inputs / Fields

- **Style:** 32px min-height, 8px radius, overlay-l1 ground, hairline border. Placeholder in ink-tertiary.
- **Focus:** Border thickens to 2px and steps to `--border-neutral-l3`. The ground does not change — the field stays the same color while focused, so typing does not flash.
- **Error / Disabled:** Error swaps the border to status-error. Disabled drops to paper-secondary ground with ink-disabled text.

### Navigation

- **Style:** 32px min-height rows, 8px radius, 16px icon, 15px label, 12px gutter between icon and text. (The gutter is written as `var(--spacer-10)` in source — note that `--spacer-10` resolves to *12px*, not 10px; the token names are pixel values from a design-tool export and only some of them match their suffix.)
- **States:** Rest is ink-secondary; active is overlay-l2 ground with ink-strongest text; hover is a lighter overlay. No left-edge indicator bar — the filled pill is the indicator.
- **Collapsed:** Below `md` the sidebar becomes a drawer; on desktop it can collapse to a 56px rail showing icons only, with the brand text and labels hidden.

### Status Indicator (signature)

The system's most-used custom component: a single inline chip that renders one of four states (`idle` / `loading` / `done` / `indeterminate`) from a `current`/`max` progress pair. Colour encodes state, but the component always carries a text label — "已登录" / "登录中" / "未登录" — so the state survives greyscale, colour-blindness, and a screenshot pasted into a chat. (In the loading and indeterminate states the badge text is hidden by CSS and the label moves to `.ds-status__progress-text`; the words are still on screen either way.) When the range is indeterminate it animates a bar back and forth rather than guessing a percentage.

**Known contrast debt (recorded, not fixed):** the `done` and `idle` badges paint `-status-*-default` text on a 12% `-surface-l1` ground of the same hue — measured 2.74:1 and 3.34:1 against the composited background, both under 4.5:1. The tag component solved this with the paired `-on-surface` step (which reaches ~5.8:1), but the status indicator was never migrated. This is the system's clearest existing accessibility gap and is tracked separately; **do not** copy the status indicator's colour pairing into new components — copy the tag's.

### Tables

Dense by default: 15px cells, 13px headers in ink-tertiary (uppercase outside the dashboard; the dashboard's in-panel headers drop the uppercase and step down to 11px), hairline row separators, first column promoted to ink-strongest. Numeric columns right-align with tabular figures in the mono face. Panel tables compress vertical cell padding to 6px, which lands body rows at 35px (22px line-height + 12px padding + 1px rule); the panel header row is a fixed 28px. A status table that shows 8 rows without scrolling beats one that shows 5 comfortably.

## Do's and Don'ts

### Do:
- **Do** spend colour on state and let chrome stay grey.
- **Do** use `tabular-nums` and a mono face for every changing figure.
- **Do** give every status a text label alongside its colour.
- **Do** keep controls at 8px radius and containers at 12px.
- **Do** pair a status `-surface` ground with its matching `-on-surface` label; the pairs are contrast-tested.
- **Do** separate with a hairline or a tonal step — pick one.
- **Do** specify light and dark together; both are first-class and a token added to one must be added to all three theme blocks.
- **Do** reference tokens (`var(--x)`) for every colour, spacing, and radius value.

### Don't:
- **Don't** use brand violet for ordinary buttons, switches, or tags — it is identity, not affordance.
- **Don't** flip rise/fall to the Western convention. Red is up.
- **Don't** add an icon or hue to a surface that is merely important; use weight or position.
- **Don't** enlarge type past 32px inside the app shell to signal importance.
- **Don't** animate scale, lift, or bounce on hover — 120ms colour and background transitions only.
- **Don't** add a shadow to something that does not float and cannot be dismissed.
- **Don't** write literal hex, px, or radius values in component code; a lint rule fails the build.
- **Don't** introduce a breakpoint outside the whitelisted five.
- **Don't** use the generic disabled ink on an inverted or coloured ground — use the paired disabled token.
