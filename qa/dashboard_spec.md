# KIS Home Dashboard — Design Specification
> Source: https://chris-kintegratedsystems.github.io/ha-dashboard/mockup-iphone.html
> Generated: 2026-04-12

---

## Color Palette

| Token | Hex | Use |
|-------|-----|-----|
| Primary Background | `#070910` | Page background |
| Surface 1 | `#0b0e17` | Base cards |
| Surface 2 | `#10151f` | Elevated cards |
| Surface 3 | `#151c2a` | Active cards |
| Surface 4 | `#1c2438` | Hover/focus |
| Accent Cyan | `#00d4f0` | Active state, borders, nav indicator |
| Green | `#10d090` | Locked, closed, secure |
| Amber | `#f5a623` | Unlocked, lights on, warnings |
| Red | `#f04060` | Open/alarm, live badge |
| Blue | `#4d8ef0` | Info states |
| Violet | `#9d6ef0` | Scene accents |
| Text High | `#eef2f8` | Primary labels |
| Text Mid | `#8a9ab8` | Secondary/sub labels |
| Text Low | `#4a5570` | Disabled/placeholder |
| Glass BG | `rgba(16,21,31,0.72)` | Card backgrounds |
| Glass Border | `rgba(255,255,255,0.06)` | Card borders |

---

## Typography

| Element | Size | Weight | Tracking | Case |
|---------|------|--------|----------|------|
| Clock | 28px | 700 | -0.02em | — |
| Date | 10px | 500 | 0.12em | UPPER |
| Section Labels | 9px | 700 | 0.22em | UPPER |
| Card Titles | 14px | 600 | — | — |
| Temperature (large) | 52px | 300 | — | — |
| Small Text | 10–11px | 500 | — | — |
| Nav Labels | 10px | 600 | 0.1em | UPPER |

**Font Stack:** `-apple-system, BlinkMacSystemFont, Segoe UI` (system fonts only — no Google Fonts)

---

## Layout

- **Viewport:** 390×844px (iPhone) / 1024×1366px (iPad)
- **Content Padding:** 16px horizontal
- **Card Gap:** 12px vertical
- **Border Radius:** 14px (cards), 10px (small elements), 8px (button groups)

---

## Sections

### Header / Status Bar
- **Height:** 88px (2-row stacked)
- **Backdrop:** blur 20px, saturation 180%, border `1px rgba(255,255,255,0.06)`
- **Row 1:** Digital clock (28px bold) + date label (10px) + alarm badge
- **Row 2:** Weather icon (22px) + temperature (16px) + presence avatars (26px diameter circles)

### Quick Actions (Scenes) — 3×3 grid
- **Cards:** ~80px min-height, padding 14px 8px, border-radius 14px
- **Icons:** colored backgrounds at 12–15% opacity
- **Scenes:** Good Morning, Good Night, Away Mode, Welcome Home, Movie Time
- **Active state:** cyan border `rgba(0,212,240,0.5)`, gradient background

### Security — Locks
- **Entities:** Front Door, Back Door, Gemelli Door
- **Layout:** 40×40px icon + label + state badge
- **Locked:** `#10d090` (green)
- **Unlocked:** `#f5a623` (amber)
- **Badges:** 10px uppercase, border-radius 20px, padding 4px 10px

### Security — Garage
- **Entities:** Left Garage, Right Garage
- **Closed:** `#10d090` (green)
- **Open:** `#f04060` (red)

### System Status
- **Chips:** "All Secure", active count, average temperature
- **Layout:** horizontal chip row

### Climate Control
- **Entities:** Living Room, Gemelli Suite, Master Bedroom, Upstairs
- **Temperature display:** 52px, 300 weight
- **Setpoint controls:** +/− buttons, 38px height
- **Mode badges:** Cool / Heat / Auto — colored, 9px uppercase
- **Progress bar:** 3px height, gradient fill

### Lights
- **Groups:** Kitchen, Living Room, Outdoor, Bedrooms
- **Display:** "X / Y on" (30px number)
- **Light chips:** 30px height, amber glow when on
- **Actions:** "All On" / "All Off" buttons, 38px height

### Cameras
- **Feeds:** 3 camera cards
- **Aspect ratio:** 16:9
- **Overlays:** Live badge (red), timestamp, crosshair, scan line grid, noise texture

### Now Playing / Media Players
- **Entities:** Hatch (Ambient Study), Living Room (Stranger Things)
- **Album art:** 52×52px, border-radius 10px
- **Controls:** Prev / Play / Next (42–50px), volume slider 80px
- **Progress bar:** 3px, gradient fill
- **Inactive:** 50% opacity

### Bottom Navigation
- **Items:** Home, Climate, Lights, Cameras, Media (5 tabs)
- **Active indicator:** 28px cyan pill, 3px height, below icon
- **Icons:** 22px SVG stroke-based
- **Height:** 68px + safe-area-inset-bottom

---

## Interactive States

| State | Effect |
|-------|--------|
| Tap | `scale(0.95–0.98)` |
| Hover | Border → `rgba(0,212,240,0.5)` |
| Active | Cyan border + gradient background |
| Lights On | Amber glow animation (pulse 2s ease-in-out) |

---

## Glass Morphism

- **Blur:** 16–24px
- **Saturation:** 160–200%
- **Shine:** `rgba(255,255,255,0.03)` gradient at 160deg
- **Border:** `rgba(255,255,255,0.06)`

---

## QA Pass/Fail Criteria

Each section must match the mockup on:
1. Background and surface colors (within visual tolerance)
2. Typography — size, weight, case
3. State colors — green=locked/closed, amber=unlocked/on, red=open/alarm
4. Layout — card gaps, padding, border-radius
5. Nav bar — 5 tabs present, active indicator visible
6. Header — clock, date, weather, presence avatars all visible
7. No Google Fonts requests (check network tab)
8. No raw JS template strings (`[[[`)rendered as text
