# CHRIS_PROGRESS_LOG — Autonomous Satellite Choke Session

Session started 2026-05-27 ~06:35 CDT — autonomous mode.
Goal: fresh-boot idle ping <50ms, wake word → Grok voice round-trip → speaker out.

---

## Baseline / diagnosis (pre-v9)

- Bridge (Pi) healthy: active, port 8766 listening, no overnight errors. Not the cause.
- Phase M diagnostic code present in kis-voice-bridge main.py (lines 409–429), Pi copy MD5-identical.
- COM6 = ESP32-S3 USB Serial/JTAG (VID:PID 303A:1001).
- Idle ping latency (from Pi):
  - USB serial CLOSED, cold: **426ms avg / 1298ms max / 0% loss**
  - USB serial OPEN (cap.py reading COM6): **69ms avg / 6.5ms min**
  - USB serial CLOSED again: **147ms avg / 8% loss**
  - Signature (low min, high max, ~0% loss) + USB-state dependence = WiFi power-save / light-sleep, NOT CPU starvation.
- `satellite1-kis.yaml`: `logger: level: DEBUG`, `wifi:` has **no power_save_mode** (defaults to light on ESP32).
- Serial capture at idle: 0 lines (logger routing / idle-quiet TBD).

**Leading hypothesis: idle choke = WiFi power-save (light sleep). Fix candidate: `wifi: power_save_mode: none`.**

---

## Iterations

### v12 — 2026-05-27 ~09:10 CDT (turn-taking)
- **Findings from v11 + bridge chunking:** mic-capture-during-speech is clean (0 drops); round-trip to Grok works; **but the session closes ~6s in and TTS never plays.** Root cause: continuous full-duplex streaming (autotest streams mic the whole time, even while ARIA should be responding) → contention + the bridge keeps forwarding post-commit mic to Grok → Grok ends the turn / session dies; big send stalls (7.5s) + drops.
- **Fix (half-duplex turn-taking, 2 parts):**
  - Bridge (deployed): after the manual commit, gate mic→Grok until response.done; send `{"type":"processing"}` to the satellite. On response.done, ungate + send `{"type":"done"}`.
  - Satellite (v12 firmware): on `"processing"` → state=RECEIVING (pause mic send, keep receiving+playing TTS); on `"done"` → STREAMING (resume). One-line recv handler.
- Kept: bridge 1024B chunking (fixed the recv-buffer crash), power_save_mode, wifi-lock.
- **Result:** _pending build+flash+capture_ — expect: processing→pause→TTS plays (speaker writes)→done→resume, no read error, low drops.

### v11 — 2026-05-27 ~08:45 CDT
- **Goal:** measure session criteria (3,4,6,7,8) now that idle ping (1) is fixed.
- **Change:** aria_bridge.cpp/.h instrumentation — per-session `mic_dropped_bytes_` + `bytes_sent_` counters (logged once at stop_session, replacing per-drop spam), and `ws_send_binary_` timing (WARN if a send >50ms, criterion 4). YAML: re-added TEMP autotest (one 10s session at boot+30s, heap before/after). Keep v10 wifi-lock + power_save.
- **Result:** ✅ Big improvements + found the return-path bug.
  - ✅ **Mic drops: 0** (sent=884736, dropped=0) — WiFi-lock eliminated the v9 267-drop problem.
  - ✅ Heap pre=post=7104336 — zero leak.
  - ✅ Round-trip outbound: audio→Grok (avg_abs ~8000), Grok returns response.audio.delta.
  - ⚠️ 3 ws_send slow warnings (54/231/93ms) — occasional, minor.
  - ❌ **Session closes ~6-7s in (`WebSocket read error — stopping`), right as Grok audio returns → TTS never plays (no speaker writes).**
- **ROOT CAUSE (return path):** bridge sends each Grok-native `response.audio.delta` as ONE big WS binary frame (~17920 B). Satellite `ws_recv_frame_` has a 2048 B buffer → large frames hit the discard path, which `return -1` on EAGAIN when the frame arrives fragmented → false "read error" → session dies. Satellite never plays TTS. (The bridge's XTTS path already chunks to 1024 B; the Grok-native path doesn't — that's the bug.)
- **Fix (v12, bridge-side, evidence-forced):** chunk Grok-native audio sends to ≤1024 B in relay_grok_to_satellite (match the XTTS pattern). Low-risk, no firmware change; satellite already plays small binary frames. Phase M manual-commit untouched.

### v10 — 2026-05-27 ~08:15 CDT (extended autonomous scope)
- Power-source ruled out: wall-PD ping (avg 99.7ms) ≈ USB ping (avg 73.8ms), same spikes. Stalls are mesh/AP-side.
- **Change:** removed v9 temp diagnostics (autotest + heap monitor); kept `power_save_mode: none`; **added `fast_connect: true` + `post_connect_roaming: false`** to lock the device to one Casadekup mesh BSSID and stop roaming churn (steady-state stall hypothesis).
- Flash: USB COM6 (esptool). Branch `phase-aria/v10-cleanup`.
- **Result:** ✅ **WORKED.** Idle ping (60s, serial-closed, USB power): avg **19.2ms** (was 73.8ms), min 0.94ms, max 290ms, 0% loss. The steady-state stalls WERE mesh roaming churn (unlogged background roaming); locking to one BSSID via fast_connect + post_connect_roaming=false fixed it. **Criterion 1 (idle ping <50ms avg/60s) MET.** One residual ~290ms blip remains (rare).
- v10 flashed via COM6, hash verified, clean boot, reachable.
- Next: session criteria (round-trip, mic drops, ws_send timing, speaker writes) — need a test-session harness since v10 has no autotest. Plan v11 = re-add instrumented autotest + ws_send timing + mic-drop counter.

### v9 — 2026-05-27 ~06:55 CDT
- **Hypothesis:** idle choke = ESPHome default WiFi power-save (light) on ESP32. Latency tracked USB-serial state (USB blocks light-sleep). No FPH PM/light-sleep config found, so power_save is at ESPHome default.
- **Change:** `wifi: power_save_mode: none` (the fix). Plus TEMP instrumentation: 20s-interval heap log + on_boot autotest (start_session→6s→stop_session at boot+75s) to verify connect/send task spawn without a wake word.
- **Scope note:** power_save_mode is radio power tuning, not credentials/SSID/channel — treated as in-scope firmware iteration for the stated goal. Trivially reversible.
- **Result (serial boot capture):**
  - ✅ Heap rock-stable: 7,099,752 free; pre-session 7099596 / post-session 7099596 — **no leak**.
  - ✅ Clean boot, no boot loop (safe_mode OK), **no log spam**.
  - ✅ connect_task_ + send_task_ both spawn on session start.
  - ❌ `socket()` create FAILS ("Socket create failed", cpp:121) → no bridge connection. This is the real "wake word fires, no bridge connection" bug.
- **ROOT CAUSE of socket failure (found):** 10+ **zombie `esphome logs` processes** in the esphome container (started May26, hours old) holding 8+ ESTABLISHED TCP connections to the device API (:6053). Orphaned because `timeout … docker exec … esphome logs` kills the outer docker exec but NOT the in-container esphome process. These exhaust the device's LWIP socket pool, so aria_bridge's socket() fails. Also explains the API "connection reset" churn + device being hammered.
- **Fix:** kill the zombie connections; correct my capture methodology (kill in-container esphome logs after each use). The "choke" + "no connection" were largely external (zombie clients), not a firmware defect.
- Ping (serial-closed, zombies present): avg 69ms, min 1.4ms, max 433ms.

### v9 — POST-CLEANUP RESULTS (the big wins)
- **Killed 30 zombie `esphome logs` connections** (root cause). Socket pool freed.
- **Rebooted → autotest socket() now SUCCEEDS:** `TCP connected to …:8766` → `WebSocket handshake complete` → `ARIA session active — streaming audio`. **The "wake word fires, no bridge connection" bug is FIXED.**
- **Full Grok round-trip CONFIRMED on bridge side** (from autotest): audio chunks streamed to Grok, `Audio level avg_abs=8243 max_abs=32557` (healthy speech level — NOT too quiet), manual commit fired → **`Grok event: response.audio.delta (35840 b64)`** — Grok responded with audio. The original "Grok never responds" was downstream of the no-connection bug.
- **power_save_mode=none fixed the latency FLOOR:** idle ping min 34ms → ~2ms.
- Heap stable (no leak), clean boot, no boot loop, no log spam, tasks spawn cleanly, mic callbacks fire.
- ⚠️ 267 mic drops during the 6s autotest — largely because the bridge session ended early (~1.6s) and the satellite streamed into a stalled socket; also aggravated by the WiFi stalls below. Revisit in Phase 2 with a real session.

### REMAINING BLOCKER — periodic WiFi stalls (NOT a device-firmware defect)
- Idle ping after all fixes: avg ~70–98ms, **min ~2ms**, periodic ~900ms spikes, ~0–1.6% loss. Same pattern from Pi (eth0) AND laptop (wifi) → it's the device's WiFi segment, not the Pi.
- **Signal is STRONG: -40dB** (Ch11) → not placement/weak-signal.
- `Casadekup` is a **MESH** (5 BSSIDs, Ch 1/6/11). **Boot-time `Authentication Failed`** to the strongest BSSID (D4:6A:91:A9:B3:44) → mesh band-steering/roaming friction. Config has `post_connect_roaming: true`.
- Steady-state stalls show **no logged scans/disconnects/roaming** on the device → cause is AP/mesh/channel-side (Ch11 congestion or band-steering) and/or the **abnormal USB 5V power** (PD negotiation failed → low-power mode; can throttle WiFi). Both are outside firmware scope.
- In-scope firmware causes RULED OUT: power-save (fixed), zombies (fixed), log spam (none), heap leak (none), task issues (none).

## 🛑 HARD STOP (v9) — awaiting Chris
Reason: the only unmet Phase-1 criterion (idle ping <50ms sustained) is blocked by network/power factors that are yours to change (HARD STOP per the brief: power source / network config / placement). Everything in my firmware scope is fixed and the device functionally works end-to-end.

---

## FOR CHRIS'S RETURN

### Current state
- Device is running **v9** (`power_save_mode=none` + TEMP diagnostics still in place: 20s heap monitor + on-boot autotest that fires a 6s bridge session at boot+75s). Branch `phase-aria/v9-wifi-powersave` (pushed). v8 baseline is commit `9527c61`.
- Bridge unchanged and healthy. **The bridge-side Phase M diagnostic (manual `input_audio_buffer.commit` + audio-level logs) is LOAD-BEARING** — it's currently what makes Grok respond (server_vad still isn't firing on its own). Do NOT remove it until the VAD issue is addressed (Phase 2).
- Zombie `esphome logs` connections: all 30 killed; socket pool free.

### ⚠️ Methodology note (important)
`timeout NN docker exec esphome esphome logs …` kills the outer `docker exec` but NOT the in-container `esphome logs` process → it lingers holding a device API socket. 30 of these had accumulated and exhausted the device's LWIP socket pool (the real "choke"). Use serial capture (`.sat-diag/cap.py` over COM6) instead, or always `sudo pkill -f 'esphome logs'` after (mind the self-match footgun — don't put the literal string in the same command line).

### To resolve the residual ping/stalls (your domain)
1. **Power on normal wall PD** (not laptop USB 5V) and re-measure idle ping — rules out USB power throttling. (This also frees COM6, so use WiFi/serial-over-USB-JTAG won't be available; use `ping` from the Pi.)
2. **Mesh/router side:** Ch11 looks busy; consider pinning the satellite to one AP/BSSID or disabling band-steering / 802.11r for it. The boot `Authentication Failed` to D4:6A:91:A9:B3:44 suggests the mesh is steering/rejecting it.
3. If you want me to try it firmware-side: `wifi: power_save_mode: none` (done) + `fast_connect: true` / `post_connect_roaming: false` to lock the device to one BSSID and stop roaming churn. I held off — it's gray-area "network config" and the steady-state stalls didn't log roaming, so the evidence is weak. Your call.

### Phase 2 ear-test (when ready)
Bridge + device are ready for a real wake-word test now (connection + Grok round-trip verified). To test:
1. Have the device on (v9). Wake-word LED should light on "Hey Jarvis".
2. Capture device serial during the test (no zombies):
   `cd C:\Projects\kintegrated\customers\ha-config\.sat-diag; python cap.py --seconds 60 --out eartest.log`
3. Speak: "Tell me a short joke about software engineers", then quiet 5s.
4. Bridge-side log: `ssh -i C:\Users\Chris\.ssh\kis_cc cooper5389@192.168.51.179 "journalctl -u kis-voice-bridge --since '2 min ago' --no-pager | tail -40"`
5. Listen for TTS out the speaker (Grok native unless XTTS/RTX5080 is awake). If quiet/garbled, that's the Phase-2 gain/quality tuning.

### Clean-up still pending (do after ping resolved / Phase 1 truly green)
- Remove v9 TEMP blocks from `satellite1-kis.yaml`: the on-boot autotest (priority -200) and the `interval:` heap monitor. Rebuild = clean v10.
- The autotest currently fires a real 6s Grok session at every boot+75s — it WILL interfere with your wake-word test if you reboot and wait. Either test within 75s of boot, or I build clean v10 first.
