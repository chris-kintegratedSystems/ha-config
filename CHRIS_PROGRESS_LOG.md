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

## STATUS @ v13 (2026-05-27 ~09:25 CDT) — round-trip WORKING; 1 robustness caveat

| # | Criterion | Status |
|---|-----------|--------|
| 1 | Idle ping <50ms/60s | ✅ 12ms avg (was 426ms) |
| 2 | Connect fast | ✅ TCP+WS ~1s |
| 3 | Zero mic drops/session | ✅ 0 (was 267→1.12MB; turn-taking fixed it) |
| 4 | No ws_send >50ms | ⚠️ near — v13 only 2 blips (96/114ms), was 30+; occasional wifi-spike |
| 5 | No API drops at idle | ✅ |
| 6 | Round-trip incl response.audio.delta + relay back | ✅ |
| 7 | Speaker plays TTS (proof-by-log) | ✅ "Starting Speaker" + ring buffers |
| 8 | Heap stable across sessions | ✅ stable across ~6 sessions (not formally 20×) |

**CAVEAT — close-time WS framing glitch:** at session end the satellite occasionally misreads a binary audio frame as a text frame (garbled "Bridge status: ▒▒") → `WebSocket read error` close. The conversation completes (TTS plays, 0 drops) *before* this; it's a robustness nuance in the hand-rolled lwIP WS parser (`ws_recv_frame_`) under bidirectional load, surfacing at termination. Proper fix = harden the frame parser (recv-path rewrite) — deferred for a real ear-test first (may not matter mid-conversation).

**Deployed:** satellite v13 (`phase-aria/v13-send-smoothing`), bridge `fix/satellite-audio-chunking` (chunking + v12 turn-taking gating). Both pushed.

## FOR CHRIS'S RETURN

**Bottom line:** the full voice round-trip works end-to-end in logs — wake/connect → mic→Grok (clean, 0 drops) → Grok returns audio → satellite speaker plays TTS → turn-taking (pause mic while ARIA talks) → resume. Idle ping fixed (426→12ms). The big original blocker (the "choke") was zombie `esphome logs` sockets, now fixed. What remains is **real-world validation (your ear-test)** + one cosmetic close-time framing artifact.

**Firmware journey (all branches pushed):**
- v9 `power_save_mode=none` → ping floor fixed.
- v10 `fast_connect + post_connect_roaming=false` → idle ping 74→12ms (mesh-roaming churn was the residual stall).
- v11 instrumentation → mic clean during speech; found TTS recv-buffer crash.
- bridge `fix/satellite-audio-chunking` → chunk Grok audio to 1024B (satellite 2048B recv buffer).
- v12 half-duplex turn-taking (bridge gates mic + sends `processing`/`done`; satellite pauses on `processing`) → round-trip + TTS playback + 0 drops.
- v13 cap satellite send to 2048B → ws_send smooth (30+ slow → 2).

**Ear-test (the real validation):** the satellite currently runs an on-boot AUTOTEST that fires a real session at boot+30s. To avoid collision, test within 30s of a fresh boot, OR ask me to build a clean v14 (strip autotest). Then:
1. "Hey Jarvis" → speak a prompt → listen for TTS out the speaker.
2. Bridge log: `ssh -i C:\Users\Chris\.ssh\kis_cc cooper5389@192.168.51.179 "journalctl -u kis-voice-bridge --since '2 min ago' --no-pager | tail -40"`
3. Watch for: response.audio.delta, and whether TTS is audible + clean. (On USB-5V the speaker is low-power/quieter; wall-PD = full volume.)

**Known remaining items:**
1. Close-time WS framing glitch (cosmetic, at session end — likely a close-race; confirm it doesn't bite mid-conversation in the ear-test). Proper fix = harden `ws_recv_frame_`.
2. Criterion 8 heap: stable across ~6 sessions; a formal 20×-session stress not run.
3. The bridge Phase M manual-commit is still LOAD-BEARING (server_vad doesn't fire) — the real turn-taking trigger should eventually be server_vad (Phase 2), not the 30-chunk manual commit.
4. Temp code to strip for final clean firmware: on-boot autotest (yaml). The instrumentation (session stats, ws_send timing) is good operational logging — recommend keeping.
5. ⚠️ Methodology: never `timeout … docker exec … esphome logs` (orphans → socket-pool exhaustion = the original choke). Use serial `.sat-diag/cap.py`.

## ACOUSTIC TEST HARNESS (sat-test/) — 2026-05-27 ~13:10 CDT

End-to-end rig that exercises the REAL satellite (the WS-only harness bypasses it):
laptop speakers play a wake-word+question prompt → satellite mic → bridge → Grok → bridge
→ satellite speaker → laptop mic captures it. Compare laptop-mic capture vs bridge-sent reference.

- **Part 1** (kis-voice-bridge/main.py): `relay_grok_to_satellite` saves exact bytes sent to the
  satellite → `/tmp/sat-tts-capture/<iso>_bridge_sent.wav` (48kHz/16bit/stereo, observer-only). Deployed.
- **Part 2** `mic_capture.py` — `MicCapture` class (sounddevice, 16kHz mono). Mic = Anker PowerConf C20.
- **Part 3** `prompt_player.py` — `play_prompt()`. Speaker = BO Speaker (Beolit 20).
- **Part 4** `sat_acoustic_test.py` — orchestrator: reset sat → mic record → play prompt → poll bridge
  for session end → stop → scp bridge_sent → STT both (faster-whisper) → metrics → trim prompt portion →
  compare (STT difflib>0.8, duration within 20%, mic LOOKS_REAL) → PASS/FAIL.
- **Part 5** `sat_acoustic_batch.py` — runs time/weather/locks sequentially.

**ACOUSTIC SETUP (Chris must maintain):** satellite within ~1-2 ft of laptop mic; laptop speakers within
~1-2 ft of satellite mic; quiet room (no other voices/TV/music); laptop volume high enough for the
satellite mic; satellite at default volume (USB-5V = TAS2780 low-power = quieter than wall-PD).
Future (Phase 2): fixed-position calibrated rig + ambient-noise-floor measurement.

**BLOCKERS before first acoustic test:**
1. Prompts are QUESTION-ONLY (STT: "What time is it?", "What's the weather like right now?", "Are all the
   doors locked?") — NO "Hey Jarvis" prefix. The acoustic path needs the wake word. Chris must re-record
   as full utterances ("Hey Jarvis, what time is it?"). No programmatic splicing (wake-word-detector artifacts).
2. Satellite must run a NO-AUTOTEST build (current v16 fires the autotest at boot+30s, which blocks the
   wake-word path). Flash a clean v16 (recv fix unchanged, autotest stripped) before the acoustic test.

## Iterations

### v15 — 2026-05-27 ~11:25 CDT (CHIPMUNK FIXED)
- **Root cause of chipmunk (traced):** the I2S bus is **stereo** (XMOS master, 48kHz/32bit/2-slot). The i2s_audio speaker expands 16→32bit but writes samples **sequentially without mono→stereo duplication** — so MONO audio is read as L,R,L,R → 2 samples/frame → **2x too fast**. Rate (16k vs 48k) was a red herring; both v13/v14 delivered mono to a stereo bus.
- **Fix:** bridge sends 48kHz **STEREO** (new `mono_to_stereo_16bit`, duplicate L=R); satellite `set_audio_stream_info(16, 2, 48000)`. ✅ **Chris confirmed pitch correct.**
- **Clipping (Chris: "clipped before speech finished"):** traced to the **autotest's 12s `stop_session`** cutting Grok's slow-to-start response (commit ~3s + Grok ~6.7s to first audio = audio starts ~10s, autotest stops at 12s → ~2s played). Removed autotest → clean v15 → real session stays open until Grok done.
- Note: Grok takes ~6.7s from response.created to first audio delta (latency, possibly because committed audio is ambient noise) — watch in real speech.
- **Result:** clean v15 built/flashing — pending Chris real ear-test (full response, no 12s cutoff).

### v14 — 2026-05-27 ~09:33 CDT (chipmunk fix + clean build)
- **Ear-test feedback:** TTS played as chipmunk/fast → ~3x too fast = 16kHz audio reaching the 48kHz I2S speaker without upsampling.
- **Fix:** bridge now resamples Grok 24kHz→**48kHz** (new `resample_24k_to_48k`, both Grok-native + XTTS send paths) and the satellite `set_audio_stream_info(16,1,48000)` — plays at I2S-native 48kHz, correct pitch. No reliance on the in-path resampler doing 16→48.
- **Also:** removed the on-boot autotest (clean build for Chris's real ear-test). Kept all v9-v13 fixes + instrumentation.
- Bridge deployed + restarted (48kHz). Firmware = v14.
- **Result:** _pending Chris ear-test_ (no autotest now; trigger via real wake word).

### v13 — 2026-05-27 ~09:25 CDT
- **Change:** satellite send_task caps each ws_send to 2048 B (loop to drain) — smooths sends, breaks the backpressure spiral.
- **Result:** ws_send >50ms 30+→**2** (max 114ms). 0 drops. Speaker plays. Heap stable. Clean turn-taking. Residual: close-time framing glitch (see caveat).

### v12 — 2026-05-27 ~09:10 CDT (turn-taking)
- **Findings from v11 + bridge chunking:** mic-capture-during-speech is clean (0 drops); round-trip to Grok works; **but the session closes ~6s in and TTS never plays.** Root cause: continuous full-duplex streaming (autotest streams mic the whole time, even while ARIA should be responding) → contention + the bridge keeps forwarding post-commit mic to Grok → Grok ends the turn / session dies; big send stalls (7.5s) + drops.
- **Fix (half-duplex turn-taking, 2 parts):**
  - Bridge (deployed): after the manual commit, gate mic→Grok until response.done; send `{"type":"processing"}` to the satellite. On response.done, ungate + send `{"type":"done"}`.
  - Satellite (v12 firmware): on `"processing"` → state=RECEIVING (pause mic send, keep receiving+playing TTS); on `"done"` → STREAMING (resume). One-line recv handler.
- Kept: bridge 1024B chunking (fixed the recv-buffer crash), power_save_mode, wifi-lock.
- **Result:** ✅✅ **FULL ROUND-TRIP WORKS.**
  - ✅ Turn-taking cycles: satellite gets `processing`→RECEIVING, `done`→STREAMING.
  - ✅ **Speaker starts + plays TTS** (`Starting/Started Speaker`, resampler + ring buffers created) — criterion 7 proof-by-log.
  - ✅ **Mic drops = 0** (sent=935936) — half-duplex eliminated the contention.
  - ✅ Heap stable, no mid-session crash (end read-error is the normal ~9.6s close).
  - ✅ Grok round-trip: response.created→done→audio.delta, relay back received.
- **7/8 criteria green.** Gap = criterion 4: many ws_send >50ms (large 50KB+ frames). Cause: backpressure spiral — send_task drains the WHOLE mic buffer in one frame; one slow send (wifi blip) grows the buffer → bigger/slower next send.
- Minor: one garbled text frame + `aria_bridge took 110ms` (speaker.play blocking loop) at session end — low impact, watch.
- **Next (v13):** cap satellite per-send size (drain in ≤2KB frames, loop) to keep each send <50ms and break the spiral; bump bridge TTS chunk 1024→2048 to halve send count.

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

---

## Future Phases (not in current v17 scope)

### Phase 2 — Hardware audit + audio pipeline optimization

Scope: identify and enable Satellite1 hardware/processing capabilities we're not currently using. Paired engineering project between Chris + Claude planning layer with CC executing specific investigations. NOT autonomous work.

Investigation list:

1. XMOS v1.0.3 firmware feature audit
   - Read FPH Satellite1-XMOS GitHub repo
   - Document which DSP features are in v1.0.3 binary:
     * Acoustic echo cancellation (AEC)
     * 4-mic array beamforming
     * Noise suppression / reduction
     * Automatic gain control (AGC)
     * De-reverberation
     * Direction-of-arrival estimation
   - For each feature: is it on by default or needs ESP32 I2C control to enable?

2. FPH package audit
   - 11 FPH packages exist; KIS imports 2 (core_board, components.external) + adds memory_flasher in v17
   - One-line annotation per remaining package: what it provides, why we exclude it or what we'd gain by including
   - Specifically check: satellite1.base.yaml audio config blocks (AEC enable, beamforming target, NR levels), LED ring control patterns, mmWave LD2410/LD2450 integration, temp/humidity/luminosity sensor exposure, hardware mute button wiring, programmable button mappings

3. FPH stock vs KIS comparison
   - Flash FPH stock firmware, run acoustic harness with same prompts, capture mic+TTS WAVs
   - Compare bridge-side audio quality KIS vs stock
   - If stock sounds cleaner, identify the delta

4. Challenging conditions testing
   - Background noise (HVAC running, music in another room, dishwasher)
   - Distance variations (1ft, 5ft, 15ft from satellite)
   - Multi-speaker scenarios (TV + person talking)
   - Quiet whisper vs normal speech vs loud speech

### Phase 3 — Full-duplex with barge-in

Scope: enable natural conversation interruption. User can say "stop" mid-TTS, satellite cancels Grok response immediately.

Requirements (depends on Phase 2 findings):
1. XMOS AEC verified working — satellite mic doesn't hear its own speaker output
2. Remove v12 half-duplex turn-taking gating (RECEIVING state transition)
3. aria_bridge forwards mic continuously, no awaiting_response gate
4. Bridge handles Grok response.cancelled events when server_vad fires speech_started during active response
5. Satellite stops i2s_audio_speaker playback when bridge sends "cancel" message
6. Network capacity validation for sustained bidirectional audio

### KIS deployment runbook (lesson from v17)

When deploying KIS satellite1 firmware to new clients:
- Must include memory_flasher block pointed at XMOS firmware
- v17 added this to satellite1-kis.yaml — future builds inherit
- Document in customer-facing deployment guide that initial boot may take longer due to XMOS auto-flash (~30-60s)
- Validation step: confirm "XMOS Firmware Version: vX.Y.Z" in boot log, not "not responding"

### Why these exist as future phases

Phase 1 (v17) gets single-turn voice working in a quiet room — the minimum viable voice assistant. Phase 2 and 3 are the work that makes it actually good (vs barely functional). Both are required before KIS client deployments — clients won't tolerate "works in a quiet room, fails when AC turns on."

---

## v17 (2026-05-27 ~16:00 CDT) — XMOS "dead" narrative REVERSED; real root cause is the mic format

### ⚠️ Correction: the XMOS was never dead. Today's earlier "smoking gun" was a misread.
- The boot log line `XMOS Firmware Version: XMOS not responding` is a **logging artifact**, NOT a failure. In `satellite1.cpp`, `status_string()` returns "XMOS not responding" for ANY non-connected state, and it's printed during `setup()` when state is always `SAT_DETACHED_STATE`. The actual version-read failure logs `"Requesting XMOS version failed"` (a WARN) — **that warning is absent from every boot log**.
- The XMOS is **alive and at v1.0.3**: `on_xmos_connected` only fires after `check_for_xmos_()` confirms a non-zero version read (it fired), and `memory_flasher.match_embedded()` returned true (version == embedded v1.0.3).
- **Lesson:** don't infer a hardware failure from one ambiguous log line. Read the component source / get ground-truth data before declaring a root cause. This misread cost two firmware build cycles.

### v17 changes (KEEP — good defensive infra, but NOT today's fix)
- Added `memory_flasher` (embed v1.0.3, verbatim from FPH base.yaml) + `http_request` dep + `on_xmos_connected` version-mismatch auto-flash, counter-guarded ≤3, to `satellite1-kis.yaml`. Branch `phase-aria/v17-xmos-flasher`.
- It is harmless (correctly skips flashing since the XMOS already matches v1.0.3) and is the right defensive mechanism for future XMOS updates / blank-chip recovery (see KIS deployment runbook above). It did NOT fix the mic.

### REAL root cause (verified, code-level): aria_bridge reads the mic at the wrong layer
- FPH `sat1_microphone` delivers, to a **raw `add_data_callback`**: `int32` (32-bit), **stereo-interleaved**, 16 kHz (decimated 48→16k, not channel-aware). See `esphome/components/satellite1/microphone/sat1_microphone.cpp` (~line 232: `samples_read = bytes_read / sizeof(int32_t)`, delivers the int32 buffer).
- FPH's audio consumers NEVER use that raw callback — `voice_assistant`/`micro_wake_word` use the `microphone:` sub-config (`channels: N` + `gain_factor: 6`) → ESPHome's MicrophoneSource wrapper extracts ONE channel, applies gain, outputs clean **16-bit mono**.
- KIS's `aria_bridge` (`microphone_id: sat1_mics`) taps the **raw callback** and ships bytes unchanged; `bridge/main.py` reads them as **16-bit mono 16 kHz** (`np.int16`, `resample_16k_to_24k`). 32-bit-stereo → 16-bit-mono = scrambled bytes → the constant ~8000 that doesn't track speech → server_vad never fires → Phase-M force-commits noise → Grok hallucinates. **This explains the bridge-audio symptoms that were wrongly blamed on the XMOS.**

### Audit deltas (FPH vs KIS), block by block
1. **Mic consumption** — FPH: channel-select + gain + 16-bit (MicrophoneSource). KIS: raw int32 stereo callback, zero conversion. ← THE BUG.
2. **micro_wake_word** — IDENTICAL (channels:1, gain_factor:6). Not a delta.
3. **sat1_mics source** (48k/32bit/stereo/GPIO15/i2s_shared) — IDENTICAL.
4. **i2s_audio (i2s_shared)** — IDENTICAL (core_board.yaml).
5. **Speaker chain** — KIS = FPH minus the mixer + pcm5122 line-out DAC; `i2s_audio_speaker`(48k/32bit/stereo)+resampler(48k/16bit) match. Not a garble cause.
6. **XMOS audio-pipeline init** — no explicit post-connect XMOS audio-config commands found in FPH (firmware auto-configures). No clear delta.

### Still UNEXPLAINED (do not conflate with the mic-format bug)
- The wake word did NOT fire on 2 clear playbacks in the v16 test, yet `micro_wake_word` config is correct (channels:1, gain:6 — identical to FPH, uses the proper wrapper, so it should get clean channel-1 audio). Open question: is the XMOS audio on channel 1 actually clean (XMOS DSP fine) or is there a deeper audio issue? Needs real ground-truth capture of channel 1 vs what aria_bridge ships.
- avg_abs constant ~8000 even when nominally quiet — consistent with int32-as-int16 misread, but confirm with a byte-level capture of the satellite's mic frames.

### Recommended next (firmware change — awaiting Chris's go)
1. Capture real bridge mic bytes; confirm they are int32 (predicted by the code).
2. Fix `aria_bridge` to extract one channel + convert int32→16-bit mono (replicate MicrophoneSource behavior: e.g., take channel 0 like voice_assistant, q31→int16), instead of shipping the raw callback bytes. Then retest wake + round-trip.

---

## v18 autonomous loop (2026-05-27 ~18:00–19:05 CDT)

Branch `phase-aria/v18-test-trigger` (firmware), `phase-aria/v18-bridge` (bridge). Auto-diagnosed via the per-session capture daemon + STT chain analyzer (`sat-test/auto_capture_daemon.py`, `session_analyzer.py`).

**Wake-word trigger blocker (carried over):** played audio (recorded OR clean XTTS-TARS) does NOT fire micro_wake_word even loud at 12" — only live human voice does. Acoustic coupling confirmed fine (Chris). It's the MWW neural-net rejecting played/synthetic audio at the 0.97 cutoff. Hypothesis C (lower cutoff to ~0.7) is the lead fix — NOT YET TESTED. To unblock autonomous testing, added a **TEST-ONLY wake-bypass**: a 90s `interval` in satellite1-kis.yaml that calls `aria.start_session()` when idle. REMOVE before production.

**v17 committed** (`4197fa7`): memory_flasher (XMOS already at v1.0.3, dormant). KEEP.

**Bridge XTTS capture gap fixed** (`5b0bd91`): `bridge_sent.wav` only captured the Grok-native path; with XTTS backend it was empty. Now captures the XTTS TTS too → step5→step6 measurable.

**#1 speaker garble (underrun) — FIX IMPLEMENTED, underrun resolved, intelligibility unconfirmed** (`7dcced6`): `aria_bridge.cpp loop()` read one ~5ms WS frame/iteration (~3× slower than 48k real-time) → 500ms speaker buffer underran → ~3.8× stretch + stutter ("ee ee"/looping). Fix: drain up to 24 frames/loop, hold un-accepted bytes in `spk_pending_` (backpressure, no drop/block). Result: duration ratio 3.76×→~1×, non-silent 6%→46% (continuous, not sparse). BUT: USB-5V speaker too quiet (s6 peak 1542 vs TTS 9149) → STT can't confirm intelligibility (the >0.85 criterion needs PD power or Chris's ear). And sessions now close fast (~3.5s, the v13 close-time framing glitch) which may truncate the TTS tail — needs follow-up.

**#3 session timeout — FIXED** (`146cb09`): `send_task_` refreshed `last_activity_ms_` on every mic-send; continuous mic streaming kept sessions alive forever (active_sessions piled to 2, daemon tracking broke). Now refreshed only on TTS-recv → sessions end (active_sessions returns to 0; were 77s, now ~3.5–13s). Unblocked clean session boundaries for the capture framework.

**#2 half-duplex, #4 Phase-M demotion: NOT STARTED.** **#5 528Hz notch: deployed earlier, deprioritized.**

**Measurement limit:** the quiet USB-5V speaker (TAS2780 low-power) makes autonomous STT validation of the speaker chain unreliable. PD power (louder amp) or Chris's ear-test is needed to confirm #1. The capture framework + analyzer otherwise work well (auto-diagnosed the garble end-to-end).

### v18 follow-up (2026-05-27 ~20:20 CDT) — capture-truncation fix + #1 VALIDATED

**Daemon capture bug fixed:** the daemon started recording laptop_mic ~1s AFTER 0→1, missing the prompt + front of every session (so all prior step6 captures were the back-half only — partially invalid). Rewrote `auto_capture_daemon.py` with a continuous rolling ring buffer: always-on InputStream → 30s ring; on session start dump 10s pre-roll + append live; on session end +5s post-roll. Now captures the full session. (Also: continuous stream sidesteps Windows mic AGC ramp; logs the resolved input device = Anker PowerConf C20.)

**Power/flash note:** satellite is on the PD wall adapter now → **COM6 is gone, flashing is OTA-only** (`esphome upload --device 192.168.51.245` via the Pi). Boosted speaker DAC volume 0.55→1.0 (`dac_proxy->set_volume(1.0)` in on_boot) and disabled the autotest interval (86400s) so live-wake tests aren't contaminated. OTA'd successfully.

**Bug #1 (speaker garble) — ~~VALIDATED FIXED~~ RETRACTED: stutter fixed, FIDELITY UNCONFIRMED/POOR (see HARNESS RECALIBRATION below)** — on a clean live round-trip (session 20260527T201825, "Hey Jarvis, what time is it?"): speaker plays the TTS **intelligibly** — s6 STT "…18pm Wednesday, May 27th" (peak 12037, loud) matches s5 TTS "He's 18 TM Wednesday, May 27th." vs the old "ee ee"/3.8× stretch. step1→step2 = 1.0. The drain fix cured the underrun. (step5→step6 scored 0.63 — a SCORING ARTIFACT: laptop_mic now includes the prompt + ~30s post-response silence while bridge_sent is response-only; the response portions match. Analyzer should compare just the response window for a true number.) **First clean end-to-end voice round-trip achieved.** #2 (half-duplex) + #4 (Phase-M demote) now have trustworthy capture and are next.

---

## HARNESS RECALIBRATION (2026-05-27 ~20:30 CDT) — HARD STOP on bug work

Chris: "Until you can trigger the satellite with my laptop speaker and properly cue the recordings, we have no trustworthy test harness." Correct. Today's PASS/DEGRADED/GARBLED numbers are suspect (truncated captures + muffled audio + scoring artifacts). **Phase 1 is NOT done.**

**Honest status of today's "fixes":**
- **#1 speaker garble:** stutter FIXED (drain, 7dcced6); **fidelity UNCONFIRMED/POOR** (muffled, multi-source — see Diag A). "Whisper can transcribe it" ≠ validated. Premature call retracted.
- **#3 session timeout:** sessions END (146cb09) but **mechanism inconsistent** — 37.9s clean timeout in session 201825, yet ~3.5s "close-glitch" in earlier sessions; the fast close MAY TRUNCATE the TTS tail. Needs audit.
- **Captures:** rolling-buffer daemon captured full prompt+response in 201825 (timing audit), but earlier sessions were truncated; muffled fidelity makes ear-validation unreliable.

**Diagnostic findings (session 20260527T201825):**
- **A (audio quality):** MUFFLING (HF rolloff), not noise (SNR 26–29dB, 0% clip). Rolloff85: prompt via laptop mic ~2.5kHz; **digital TTS (bridge_sent, pre-speaker) ~1.6kHz** (XTTS TARS voice is low-bandwidth at the SOURCE — possibly the deep TARS voice character or an XTTS quality setting); satellite speaker output ~0.8kHz. TTS source is the dominant limiter for the response; laptop mic limits input capture.
- **C (timing):** connect 20:18:25 → commit :26 → response.created :28 → response.done+XTTS :31 → ended (37.9s) 20:19:03. laptop_mic (53.3s) contains full prompt (8.5–10.6s) AND full TTS (16.3–19.0s ≈ 3.1s TTS, NOT truncated). Long file = 10s preroll + ~30s post-TTS dead air (timeout) + 5s postroll. Nit: 30s post-TTS timeout too long.

**NEW TASK — Harness reliability (3 requirements, ALL required before bug validation):**
1. **Laptop speaker reliably triggers the satellite** (BLOCKING). micro_wake_word 0.97 cutoff rejects played audio. Try 0.97→0.85→0.7; check FPH's automated wake-test approach; consider a test-mode threshold. (Autotest interval kept as fallback.)
2. **Capture timing gap-free** (timeline audit + daemon fixes; full prompt + full TTS tail every session).
3. **Audio-fidelity baseline** — quantify independently: (A) sat-mic chain (Chris voice→sat_mic_raw), (B) speaker chain (bridge_sent→laptop_mic), (C) laptop mic as reference (Anker C20 baseline).

**NEW TASK — LED state machine (Phase-1 close-out):** KIS replaced `voice_assistant` (FPH's `control_leds` state machine, called on listening/thinking/replying/end) with `aria_bridge`, which has **ZERO LED control**. Only action is one-shot `light.turn_on "Listening"` (rainbow) on wake → nothing turns it off → **LED hangs forever** (no idle/thinking/error). Fix = aria_bridge drives the LED (state callbacks or is_active polling).
**Desired LED behavior (design target):** IDLE = off · WAKE/LISTENING = blue slow breathing pulse · THINKING/PROCESSING/RESPONDING = blue rotating chase · ERROR = red · returns to IDLE cleanly on session end.

### Option 2 (bridge-side mic-format fix) — IMPLEMENTED & DEPLOYED, but UNVALIDATED (blocked by wake word)
- `kis-voice-bridge` branch `phase-aria/fix-mic-format` (commit 1810586): added `sat_mic_to_pcm16()` in `bridge/main.py` — raw bytes → int32 LE → reshape stereo → channel 0 → Q31→Q15 (`>>16`) → ×6 gain w/ saturation → 16-bit mono; used for both the audio-level logging and the resample→Grok path. Deployed to the Pi + service restarted clean.
- **ARCHITECTURAL LESSON (the real takeaway):** when replacing `voice_assistant` with a custom integration, ESPHome's MicrophoneSource wrapper layer (`channels:` + `gain_factor:`) is **NOT optional** — it does the channel-extraction + int32(Q31)→int16(Q15) + gain that the raw `add_data_callback` skips. `aria_bridge` consumed the raw callback and shipped int32/stereo bytes that the bridge read as 16-bit mono → garbage. Custom integrations MUST replicate this conversion explicitly (done here bridge-side; the proper fix is firmware-side in aria_bridge — Option 1, deferred).
- **NOT validated end-to-end:** ran `sat_acoustic_test.py time` — `session ended: False`, `bridge_sent: None`. **The on-device wake word did not fire** (same as the v16 test), so no session started and no mic audio reached the bridge. The fix is code-correct + deployed but can't be confirmed until a session exists.

### CURRENT BLOCKER: on-device wake word not firing (the still-unexplained thread)
- micro_wake_word config is correct (channels:1, gain_factor:6, identical to FPH, uses the proper wrapper) and the boot log shows it running (`DETECTING_WAKE_WORD`, inference task running). Yet 3 clear "Hey Jarvis" playbacks (v16 ×2, fix-test ×1) produced no detection / no session. Bridge-side fix does not touch on-device wake detection.
- Ranked hypotheses: (a) acoustic coupling — satellite mic isn't receiving the BO-speaker playback strongly enough for the strict 0.97 cutoff (positioning/volume/orientation); (b) wake-channel layout — micro_wake_word reads channel 1; if the room mic is on a different channel on this XMOS build, it hears nothing; (c) XMOS mic-audio quality on the wake channel.
- Recommended escalating diagnosis (build-free first): (1) tighten acoustic coupling — BO speaker right at the satellite mic, high volume — and re-run; if it fires, coupling was it and we can finally validate the mic-format fix. (2) If still no fire: a diagnostic firmware build that auto-starts an aria_bridge session (bypass wake) + logs both mic channels' levels — validates the mic-format fix independently AND reveals the channel layout / whether the satellite mic captures real room audio.

---

## 2026-05-27 evening — Bug #2 validated + LED state-machine characterization

### Bug #2 (half-duplex feedback) — FIXED & VALIDATED (Option A, v20)
- **Firmware (aria_bridge.cpp / .h, v20):** added `last_spk_play_ms_` + `playback_complete_sent_` atomic state; new `ws_send_text_()` + `send_playback_complete_()`; loop() emits `{"type":"playback_complete","ts":<millis>}` once when `spk_pending_.empty() && (now - last_spk_play_ms_) >= 700 ms` (500 ms i2s drain + 200 ms acoustic margin). Reset on every `spk_->play()` with bytes accepted, and on `start_session`/`stop_session`. Build SUCCESS (59 s), OTA flash SUCCESS (4.71 s).
- **Bridge (main.py, v20):** gates EVERY response on `response.created` (not just manual-commit); `response.done` no longer releases the gate or sends `{"type":"done"}` to satellite — instead arms a rolling 10 s failsafe from `max(response_done_t, last_audio_sent_t)`. New `playback_complete` text-frame handler: 200 ms acoustic margin → send `{"type":"done"}` → release gate → cancel failsafe. Cleanup cancels the failsafe task in the session finally block.
- **Validation session (21:53:57 → 21:54:32, 35.0 s):** journal scorecard — **1 response.created · 1 response.done · 1 input_audio_transcription.completed** (`grep "type" | sort | uniq -c`). `playback_complete` fired at 21:54:02.903 (1.32 s after response.done); gate released at 21:54:03.104 (exactly +200 ms). Session ended naturally at 30.65 s after response.done (inactivity timeout — **not** the 75 s daemon cap). Ear test: one clean response — Chris signed off.
- **Caveat (no functional impact):** one `speech_started` fired 98 ms after gate release (residual room decay just outside the 200 ms margin). It produced **no transcription and no second response.created** — the loop is broken at the response-generation layer. Levers if perfection wanted later: longer margin (400–500 ms) or `input_audio_buffer.clear` at gate release. **Not implementing — Phase 2.**

### LED state-machine — characterization (no implementation yet, awaiting decision)

**Root-cause re-confirmation via static analysis tonight:**
- `satellite1-kis.yaml` LED actions: **exactly one** — `light.turn_on id:led_ring effect:"Listening"` inside `on_wake_word_detected` (line 260-262). No turn-off, no idle/error/thinking branches. Effect "Listening" is `addressable_rainbow` (not the desired blue pulse).
- `aria_bridge` (.h + .cpp): **zero** LED references — no callbacks, no triggers, no light-component pointer. The C++ component has no ESPHome `Trigger<>` plumbing for state transitions, so YAML has nothing to wire to.
- FPH stock pattern (`config/satellite1.base.yaml` + `config/common/voice_assistant.yaml` + `config/common/led_ring.yaml`): a `control_leds` script reads a `voice_assistant_phase` global + several others (XMOS flashing state, mute state, timer state, …) and selects from 18 named effects. `voice_assistant` triggers (`on_listening`, `on_stt_vad_start`, `on_intent_progress`, `on_tts_start`, `on_end`, `on_client_connected`/`disconnected`) each call `script.execute: control_leds`. **KIS replaced `voice_assistant` with `aria_bridge`, which exposes none of those triggers → the state machine is structurally orphaned.**

**Why the LED hangs:** wake-word YAML one-shot turn_on → effect runs forever; nothing in YAML or aria_bridge knows the session ended → no turn_off ever fires.

**Fix path options (cost / risk / capability):**

| Option | Change | Time | Risk | Capability |
|--------|--------|-----:|-----:|------------|
| **A. YAML-only polling** | Add an `interval: 250ms` block in `satellite1-kis.yaml` that reads `id(aria).is_active()`: on true → `light.turn_on effect:Listening`, on false → `light.turn_off`. No firmware change. | ~10 min | very low | 2-state (active/idle); ~250 ms latency on transitions; no LISTENING-vs-THINKING distinction |
| **B. Firmware triggers + YAML wiring** | Add ESPHome `Trigger<>` plumbing in aria_bridge (`on_session_start`, `on_response_received`, `on_session_end`, `on_error`) + Python codegen schema; YAML wires `script.execute: control_leds`-style effects per trigger. Mirrors FPH pattern. | ~45–60 min | medium (codegen schema + build + OTA + re-validate Option A interactions) | Full state machine; instant transitions; matches desired spec (LISTENING blue pulse, THINKING chase, ERROR red, IDLE off) |
| **C. Direct LED control in aria_bridge.cpp** | Add `light::LightState *led_;` setter; in `start_session`/`loop()`/`stop_session` directly call `light_->turn_on()`/`turn_off()`/effect select. | ~30 min | medium (firmware change, tighter coupling, harder to extend) | Full state machine but coupled to one light entity |

**Recommendation:** **Option A tonight** to close out Phase 1 — eliminates the "looks broken" hang with minimal risk and zero firmware change. Defer **Option B** (firmware triggers + full state machine matching the desired spec) to a planned Phase 2 LED ticket. Option A gives basic active-vs-idle; Phase 2 swaps to triggers + effects for LISTENING / THINKING / ERROR distinction.

### LED state machine — IMPLEMENTED (Option B chosen, v21)

Chris approved Option B (full state machine, blue family for normal phases / red for errors). Firmware + YAML in one cycle. Build SUCCESS (59.4 s), OTA SUCCESS (7.38 s), visual validation by Chris across multi-turn sessions confirmed correct behavior.

- **Firmware (`aria_bridge.h` / `.cpp` / `__init__.py`):** added `LedPhase` enum, `CallbackManager<void()>` per phase, `fire_*()` helpers using `atomic.exchange()` so each transition fires only on entry. Five ESPHome Trigger<> classes (`ListeningStartTrigger`, `ThinkingStartTrigger`, `RespondingStartTrigger`, `ErrorTrigger`, `IdleTrigger`) wired through Python codegen schema (`automation.validate_automation` + `CONF_TRIGGER_ID`).
- **Trigger points:**
  - `start_session()` → `fire_listening_()` (LED on immediately at wake, not after WS handshake)
  - ws_recv text `"processing"` → `fire_thinking_()` (Grok generating response)
  - first binary audio frame → `fire_responding_()` (TTS playing)
  - ws_recv text `"done"` → `fire_listening_()` (back to listening for follow-up)
  - state→ERROR (loop's error branch) → `fire_error_()` (red flash before stop)
  - `stop_session()` → `fire_idle_()` (light off)
- **YAML (`satellite1-kis.yaml`):** removed direct `light.turn_on` from `on_wake_word_detected` (aria_bridge owns LED now). Added four effects on `led_ring`: `Listening Blue Pulse` (1500ms breathing, 15–60% brightness), `Thinking Blue Chase` (`addressable_scan` 80ms, width 4), `Responding Blue Chase` (`addressable_scan` 40ms, width 6), `Error Red Pulse` (200ms, 30–100%). Each `aria_bridge` trigger calls `light.turn_on` with explicit RGB + effect: LISTENING ~#4080FF mid, THINKING ~#2060FF brighter, RESPONDING ~#0040FF brightest, ERROR pure red. `on_idle` calls `light.turn_off`.
- **Validation:** Chris ear-tested + visually validated. Multi-turn follow-up window (LISTENING blue pulse after playback_complete) confirmed working.

---

## PHASE 1 CLOSE-OUT — 2026-05-27 (evening)

**Status: COMPLETE.** End-to-end voice round-trip is reliable. Wake → mic → Grok → TTS → speaker → multi-turn → idle. All five Phase 1 bugs validated and signed off by Chris.

### Phase 1 bugs — final state

| # | Bug | Status | Validation |
|---|-----|--------|------------|
| 1 | Speaker garble (stutter from one-frame-per-loop drain) | FIXED v18 (`7dcced6`) | Ear test (post-fidelity fixes) |
| 1b | Mic format (int32/stereo→16-bit mono garbage) | FIXED `1810586`, `4730598` | Grok server_vad fires; transcripts coherent |
| 3 | Session timeout (sessions never ending) | FIXED `146cb09` | Sessions end ~30 s after last TTS |
| 2 | Half-duplex feedback (satellite mic hears own TTS → echo turns) | FIXED v20 (firmware playback_complete + bridge gate-on-response.created + 200 ms margin + 10 s rolling failsafe) | Journal scorecard 4/5 strict pass + ear test (one clean response) |
| LED | State machine orphaned (rainbow on, nothing turns off) | FIXED v21 (Trigger<> codegen + 4-effect palette) | Visual across multi-turn sessions |

### Harness & infrastructure landed

- **Continuous capture** (`sat-test/continuous_capture.py`): single-script 48 kHz/stereo/WASAPI-exclusive, self-patching WAV header (valid on any kill). Chris owns start/stop, no daemon coordination.
- **Per-session daemon** (`sat-test/auto_capture_daemon.py`): 48 kHz/stereo/WASAPI-exclusive C200 with rolling 30 s ring, `DAEMON_DIAG=1` writes a full-continuous reference for slicing comparison.
- **Compare tool** (`sat-test/compare_daemon_vs_full.py`): cross-correlates session slice against full reference, detects bursts, reports inside/outside slice. **Proved the daemon is NOT the bug** (drift = 0 ms in the diagnostic run).
- **Anker C200 native path**: WASAPI exclusive bypasses Windows MME mixing + DSP enhancements off (Chris). Rolloff85 = 22 kHz vs old 16k-MME 2.5 kHz; HF>4k = 68.7%.
- **Bridge captures**: `/tmp/sat-tts-capture/<ts>_bridge_sent.wav` (48k/stereo what bridge sent), `/tmp/sat-mic-capture/<ts>_raw.wav` (raw sat int32/stereo/16k), `<ts>_to_grok.wav` (16k/mono converted). 2-second incremental flush — always valid WAV.

### Tonight's reference capture (signed off)

- Bug #2 validation: `sat-test/continuous/continuous_20260527T215339.wav` (34.7 s) + `20260527T210509_bridge_sent.wav`. Journal scorecard: 1 response.created, 1 response.done, 1 transcription. `playback_complete` at +1.32 s after response.done, gate released at +200 ms.

### Phase 2 — deferred list

(Carry forward; not blocking. Order is suggestion, not commitment.)

1. **Grok response verbosity** — system prompt's pre-injected home-state + `"Use it"` directive overrides the `"no padding"` rule. `"What time is it?"` returns time + status briefing. Fix path A: edit `config.py::ARIA_SYSTEM_PROMPT` — replace `"Use it"` with `"Reference home state ONLY when relevant to the question. Don't volunteer status unless asked."` + add a simple-factual-question rule. Single-file, ~5 min, test in one session.
2. **Spurious `speech_started` after gate release** — 98 ms after gate release one server_vad fire happens (residual room decay outside the 200 ms margin). **It produces no transcription and no response.** Levers if desired: longer margin (400–500 ms) or `input_audio_buffer.clear` at gate release.
3. **Phase M demotion** — manual commit at 30 chunks was a fallback for unreliable server_vad. Server_vad now fires correctly with the mic-format fix; manual commit is likely redundant. Audit: run a session with manual commit disabled, confirm server_vad commits the prompt.
4. **MWW threshold tuning** (currently 0.7 — was 0.97). Production target depends on use case: 0.97 rejects most played audio (live-only); 0.7 lets laptop-speaker prompts fire for autonomous testing. Calibrate against false-trigger rate.
5. **TARS XTTS voice quality** — only `tars` voice on the XTTS server, low-band (~1.5 kHz). Phase 2 — either improve the TARS clone reference (`tars_reference.wav`) or add a non-TARS voice to XTTS server.
6. **528 Hz electrical tone notch refinement** — `MIC_NOTCH_HZ=527,1054` Q=20 in `config.py`. Watch for drift, narrower Q if needed.
7. **BO Beolit 20 speaker** — music-tuned; possibly wrong for voice-prompt playback testing. Phase 2 hardware audit; recommend a voice-tuned monitor speaker for harness work.
8. **LED Phase 2 polish** — currently 4 effects. Could add: dimmer IDLE indicator (ambient blue at 5%), a brief "click" effect on tool call, a subtle "warming up" pulse during the CONNECTING gap (currently silent on the LED — wake→connect→listening is instant for fire_listening_, but if WS handshake fails, only ERROR fires).

### Reference branches & commits

- `kis-voice-bridge` on `phase-aria/v18-bridge`: v20 Option A gate logic (`main.py` half-duplex playback_complete handler + 10 s rolling failsafe).
- `ha-config` on `phase-aria/v19-mww-threshold`: v20 firmware (`aria_bridge` playback_complete signal) + v21 firmware (LED state machine Trigger<> codegen + YAML wiring) + progress log.
