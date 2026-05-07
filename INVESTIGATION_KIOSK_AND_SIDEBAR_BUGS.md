# Investigation: Kiosk Mode & Sidebar Bugs (Deferred)

**Date:** 2026-05-07
**Investigator:** Claude Code
**Status:** Deferred — two bugs documented, no fix shipped

---

## Bug 1: Kiosk CSS not applying on iPhone / Desktop browsers

### Symptom
When `input_boolean.kiosk_mode` is toggled OFF on the Tab S9 (via the
Settings page toggle), the HA chrome (header + sidebar) appears correctly
on iPad and Tab S9 but does NOT appear on iPhone HA Companion or desktop
browsers.

### What we know
- The `kiosk_mode:` block in dashboard_mobilev1.json was changed by PR #46:
  ```json
  "kiosk_mode": {
    "hide_header": "[[[is_state('input_boolean.kiosk_mode', 'on')]]]",
    "hide_sidebar": "[[[is_state('input_boolean.kiosk_mode', 'on')]]]"
  }
  ```
- The `[[[...]]]` JS template syntax is evaluated by the kiosk-mode HACS
  integration, which injects CSS into `ha-drawer`'s shadow root
- The kiosk-mode integration IS installed on the Pi
- Works on: iPad, Tab S9 (FKB)
- Does NOT work on: iPhone HA Companion, desktop Chrome/Edge

### Possible causes (not investigated)
1. **Per-device evaluation:** kiosk-mode may cache its CSS injection
   per-device and not re-evaluate when the entity changes on a different
   device
2. **Companion app caching:** iPhone HA Companion may cache the kiosk
   CSS and not re-read the template on entity state change
3. **`?kiosk` URL param residue:** If the iPhone Companion URL still
   has `?kiosk`, native HA kiosk mode overrides the integration
4. **Template evaluation timing:** The `[[[...]]]` syntax may only be
   evaluated on dashboard load, not reactively on entity change

### Investigation needed
- Check if iPhone Companion URL has `?kiosk` appended
- Check kiosk-mode integration version and changelog for reactive
  template support
- Test: toggle entity, then force-reload iPhone Companion — does the
  change appear?
- Inspect kiosk-mode source for per-device vs global CSS injection

---

## Bug 2: Sidebar non-scrollable when HA chrome visible

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
