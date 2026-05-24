# Investigation: Kiosk Mode & Sidebar Bugs (Deferred)

**Date:** 2026-05-07 (opened), 2026-05-24 (Bug 1 closed)
**Investigator:** Claude Code
**Status:** Bug 1 CLOSED, Bug 2 open (mobilev2-only)

---

## Bug 1: Kiosk CSS not applying on certain devices — CLOSED 2026-05-24

### Resolution
ha-dashboard PR #84 (kis-nav.js v54 + v55), ha-config PR #52 (cache-bust
to v55).

### Original symptom
When `input_boolean.kiosk_mode` was toggled, kiosk CSS appeared to work
on some devices (Galaxy Tab A9+ FKB, iPad Companion App) but not others
(iPhone HA Companion, desktop Chrome/Edge). Original hypothesis was that
the `[[[...]]]` kiosk-mode HACS template wasn't being re-evaluated
per-device. That hypothesis was wrong.

### Actual root cause — two parts

**Part 1:** kis-nav.js never had a kiosk CSS engine through v53. Only
kis-app-shell.js (mobilev2) implemented kiosk mode via `syncKioskMode`
and `patchHALayout`. mobilev1's kiosk toggle existed in dashboard JSON
but flipped `input_boolean.kiosk_mode` with no script on mobilev1
listening. Devices that appeared to "work" were running mobilev2 or had
stale mobilev2 CSS cached in their hui-root shadow DOM from a prior
mobilev2 session. Devices that "broke" were on a clean mobilev1 with no
kiosk engine.

**Fix (v54):** Ported `syncKioskMode` and supporting functions into
kis-nav.js with adaptations: inline `_kioskOriginals` capture with
`isConnected` staleness guard, inverse `onV2Dashboard()` guard so
mobilev1 owns kiosk on its pages while mobilev2 retains its existing
behavior, CSS ID prefix `kis-kiosk-*` to avoid collision with mobilev2's
`kisv2-kiosk-*`, boot-time pre-hide reads `localStorage` to prevent
sidebar flash on load.

**Part 2 (discovered post-v54):** kis-app-shell.js's `unpatchHALayout`
strips shared host classes (`kis-kiosk-collapsed`, `kis-kiosk-hidden`)
and inline styles (`--mdc-drawer-width`, `display:none`) on every
`location-changed` event where `onV2Dashboard()` returns false — which
includes all mobilev1 navigation. After the strip, kis-nav.js's
`syncKioskMode` dedup blocked re-application because the entity state
hadn't changed. Bug visibly manifested only on wide viewports (>870px:
iPad landscape, desktop browser) because HA's drawer defaults to
`type="modal"` below that breakpoint (sidebar hidden by default after
strip) vs `type=""` above (sidebar reappears docked at ~255px). Tab A9+
in FKB appeared fine only because the wall kiosk doesn't navigate between
mobilev1 views in normal operation.

**Fix (v55):** Dedup now checks both entity state AND class presence on
drawer — if kiosk is supposed to be ON but `kis-kiosk-collapsed` class is
missing, fall through and re-apply. Added `location-changed` and
`popstate` listeners that trigger a microtask-deferred re-apply, beating
the 1s polling tick.

### Verification
Tested on all four target devices: Tab A9+ FKB, iPad Companion App,
iPhone 16 Pro Companion App, desktop browser (1920×1080). Full navigation
cycle (Home → Climate → Lights → Cameras → Media → Settings → Home) with
kiosk ON — no chrome reappear on any device.

---

## Bug 2: Sidebar non-scrollable when HA chrome visible — OPEN (mobilev2-only)

**Scope note (2026-05-24):** This bug only affects mobilev2
(kis-app-shell.js). mobilev1 was never affected because kis-nav.js never
ran the shadow DOM injection path into `ha-sidebar` that causes the
scroll clipping. The fix attempted and reverted below was in the mobilev2
code path.

### Symptom
When `kiosk_mode` is OFF (HA header + sidebar visible), the sidebar's
panel list is not scrollable. The bottom items are cut off behind the
kis-nav bottom bar.

### Root cause (confirmed via Playwright probe)
- `aside.mdc-drawer` in `ha-drawer`'s shadow root is `position: fixed`
  at full viewport height (`100vh`)
- kis-nav's header bar (64px) and bottom nav bar (56px) overlap the
  sidebar — the sidebar extends behind both bars
- `.panels-list` inside `ha-sidebar`'s shadow root has content (820px)
  taller than the visible area (728px after bars), but no `overflow-y:
  auto` — content clips

### Fix attempted and reverted
A kis-nav.js patch was implemented in commit `e5a17c7` that:
1. Injected CSS into `ha-drawer` shadow root to offset `aside.mdc-drawer`
   by header height and constrain height to `calc(100vh - header - nav)`
2. Injected CSS into `ha-sidebar` shadow root to add `overflow-y: auto`
   on `.panels-list`

This fix passed Playwright QA (probe confirmed `canScroll: true`) but
Chris reported it did not work on real devices. Reverted in `9bee466`.

### Why the fix may have failed in production
1. **Timing:** The sidebar shadow root may not exist when
   `applyDynamicHeaderClearance()` first runs. The injection may need a
   MutationObserver to wait for sidebar mount.
2. **CSS specificity:** HA's own sidebar styles may use
   `adoptedStyleSheets` which override injected `<style>` elements at
   equal specificity.
3. **Different class names:** Production HA version may use different
   MDC class names than the Playwright-rendered version.
4. **FKB vs Chromium:** Android WebView rendering may handle
   `position: fixed` + injected CSS differently than desktop Chromium.

### Next steps
- Run a diagnostic-first approach: instrument with console.log to
  confirm the injection lands on real devices
- Check if `aside.mdc-drawer` class name matches on Tab S9 vs Playwright
- Consider alternative approach: modify kis-nav's own positioning
  instead of trying to fix HA's sidebar from outside
- Consider whether this is worth fixing at all — kiosk mode ON is the
  normal state; sidebar is only visible during setup/debug

---

## Priority assessment

Both bugs only affect the non-kiosk state. The Tab S9 wall mount runs
in kiosk mode 99% of the time. These are quality-of-life issues for
the rare case when Chris toggles kiosk OFF for debugging, not
production-critical bugs.

**Recommended priority:** Low — fix when convenient, not urgent.
