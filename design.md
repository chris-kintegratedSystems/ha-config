# Unified Event Log + Wake Diagnostics — Build Session Design

**Build start:** 2026-05-28 ~06:24 CDT
**12-hour hard stop:** 2026-05-28 18:24 CDT
**Status:** IN PROGRESS

This document is the **LOCKED source of truth** for this autonomous build session.
If reality diverges mid-build, this file is updated with the divergence + reason
in the "Divergences" section. Original assumptions are crossed out, not deleted.

---

## Context / the bug

Phantom sessions: satellite-side wake fires at `probability_cutoff: 0.7`
(`satellite1-kis.yaml:251`, lowered from FPH stock 0.97 in v19), ~every 15 min,
in a silent room, each running a full Grok session returning a status dump. Part 1
diagnostic confirmed: bridge ruled out, HA ruled out, autotest interval is 24 h,
`on_wake_word_detected` is the only ~15-min path. We are **NOT changing the cutoff
in this build** — we build the instrument to decide between:
- raise cutoff back toward 0.97 (if fires cluster near 0.71 — false-positives on
  ambient at the loose threshold)
- hunt an acoustic source (if fires cluster near 0.96 — real "Hey Jarvis"-like
  sound in the room)

Plus the audio audit so we can identify what the source actually is and tune the
pipeline long-term.

## Scope fences (DO NOT)

- Enable STT suppression. STT speech-gate is **observe-only** this build.
- Change `probability_cutoff`. The decision waits for the log data this build produces.
- Touch v22 gating (`bridge/main.py` `_release_gate_after_margin` / `_gate_failsafe`).
- Touch the verbosity prompt (`bridge/config.py::ARIA_SYSTEM_PROMPT`).
- Start any Phase 2 item not listed here.

## Power / flash policy

Satellite on USB → flash via **COM6 / esptool**, not OTA.

## Order of work

1. Pre-step: kill leftover daemon + confirm COM6.
2. **Bridge layer (no firmware risk):**
   - `bridge/event_log.py` (RotatingFileHandler-backed writer + emit API).
   - `POST /satellite-event` endpoint on 8766.
   - ~20 instrumentation calls in `main.py` at the points in the catalogue below.
   - `/events` SSE live viewer.
   - STT speech-gate observer (no suppression).
3. **SELF-VERIFY GATE** (replaces in-person checkpoint):
   - Restart bridge.
   - Temporarily lower autotest interval (24 h → e.g. 60 s) on the satellite **without
     reflashing if possible**; or run a manual trigger.
   - One autotest-fired session lands a coherent event sequence in `events.jsonl`.
   - `/events` streams it live.
   - **Paste the actual event lines into this design.md under "Bridge-side self-verify".**
   - **Proceed to firmware only if it passes.**
4. **Firmware:**
   - `time:` SNTP block.
   - `aria_bridge::emit_event_` method.
   - YAML wires for boot / WiFi / wake / phase / playback_complete.
   - **Option B** MWW probability patch.
   - **Pre-wake audio capture** (a → b → c, hard required).
   - Restore autotest interval to 24 h.
   - esphome compile → COM6 esptool flash.
5. **End-to-end validation** — all 6 criteria with evidence pasted here.
6. Commit + push + final Slack.

## Schema (locked)

JSON Lines, one record per line, written by the bridge to one file.

```json
{
  "ts": "2026-05-28T05:31:01.619-05:00",
  "ts_unix_ms": 1779967861619,
  "source": "satellite",
  "event": "wake_word_detected",
  "session_id": "abc12345",
  "satellite_id": "satellite1-kis",
  "schema": 1,
  "payload": { "wake_word": "hey_jarvis", "probability": 0.78 }
}
```

- `source` ∈ {`satellite`, `bridge`, `grok`, `ha`, `external`}.
- `session_id` is `null` for out-of-session events (boot, wake-pre-connect, WiFi).
- For bridge-recorded events `ts` is bridge arrival wall-clock; for satellite-emitted
  events `ts` is the satellite's NTP-synced clock.
- `schema: 1` carries forward — bump if breaking changes.

## File locations (locked)

- Unified log: **`/var/log/kis-voice-bridge/events.jsonl`** (mkdir + chown at service start;
  fallback `/home/cooper5389/kis-voice-bridge/events.jsonl` if permissions block).
- Per-session audio (pre-wake + session, stitched): **`/var/log/kis-voice-bridge/audio/<session_id>.wav`**.
- Rotation: 10 MB × 30 backups (~310 MB ceiling).

## Kill switch

- Bridge: env `EVENT_LOG_ENABLED=true|false` (default `true`).
- Satellite: YAML global `events_enabled` (default `true`).

## Event catalogue (locked)

**Satellite-originated** (POST to `/satellite-event`):
- `boot` — `{ version, reset_reason, free_psram_kb }`
- `wifi_connected` / `wifi_disconnected` — `{ ssid?, bssid?, rssi_dbm? }`
- `xmos_connected` / `xmos_no_response` / `xmos_flash_started/success/failed`
- `wake_word_detected` — `{ wake_word, probability }` *(probability requires Option B patch)*
- `aria_session_started` — `{ session_id }`
- `led_phase_change` — `{ from, to }`
- `playback_complete` — `{ idle_ms }`
- `aria_session_ended` — `{ session_id, duration_ms, bytes_sent, bytes_dropped }`
- `prewake_audio_uploaded` — `{ session_id, sample_count, sample_rate, channels }` (after binary POST completes)

**Bridge-originated** (`event_log.emit(...)` in `main.py`):
- `bridge_started` / `bridge_stopped`
- `satellite_ws_connected` — `{ client_addr, session_id }`
- `home_state_snapshot` — `{ blob }`
- `grok_session_opened` — `{ tools_count, voice, model }`
- `mic_audio_first`
- `mic_audio_committed` — `{ source: "manual"|"server_vad", chunk_count }`
- `transcription` — `{ source: "grok_input", text, item_id, stt_verdict, would_suppress }` *(STT observer fields added here)*
- `grok_event` — `{ etype, event_id, response_id }`
- `tts_first_byte` — `{ from: "grok"|"xtts" }`
- `tts_first_byte_sent` — `{ bytes }`
- `tts_total_sent` — `{ bytes, audio_duration_ms }`
- `response_text` — `{ text }` (untruncated)
- `playback_complete_received` — `{ satellite_ts, idle_ms }`
- `gate_state_change` — `{ from, to, reason }`
- `satellite_session_closed` — `{ reason, duration_ms }`

## STT speech-gate (observe-only, **TEMPORARY**)

**Marked temporary in code + here.** The real fix is a wake pipeline that doesn't
false-fire — found via the audio audit. STT-gate is the instrument, not the
treatment.

Classification on Grok's `conversation.item.input_audio_transcription.completed`
transcript:
- `speech`: ≥ 2 distinct content words and length ≥ 10 chars.
- `empty`: blank / whitespace only.
- `gibberish`: < 2 distinct words OR < 10 chars OR mostly non-alphabetic.

Record carries `stt_verdict` ∈ {speech, empty, gibberish} and `would_suppress`
∈ {true (for empty/gibberish), false (for speech)}. **Never actually suppresses.**

**Latency question to measure during self-verify:** does
`conversation.item.input_audio_transcription.completed` arrive BEFORE
`response.created`, or after? Determines whether future suppression is latency-free
or requires cancellation of an in-flight response. Measured value written to
"STT timing finding" below.

## Pre-wake audio investigation (path a / b / c)

Hard requirement. Investigate in order:
- **(a)** Does FPH's `micro_wake_word` already retain a pre-roll buffer (~1 s pre-wake
  so the wake word reaches STT)? If so, route it to the bridge on wake.
- **(b)** Samples exist internally but unexposed → patch to tap the existing ring.
- **(c)** Neither → build the parallel PSRAM ring buffer + flush-on-wake HTTP upload.

Each clip: ~2 s pre-wake + session audio, saved on the Pi named by `session_id`,
event record carries `audio_path`, `/events` gets a play button.

**Finding (to be updated as investigated):** TBD.

## Self-verify gate — REQUIRED before any flash

After the bridge layer is deployed and BEFORE any firmware flash:
1. Restart bridge.
2. Trigger one session (autotest temporarily lowered to ~60 s, or manual stimulus).
3. Verify `events.jsonl` populated with a coherent sequence.
4. Verify `/events` streamed it live.
5. Verify schema correct.
6. **Paste the actual event lines into "Bridge-side self-verify result" below.**
7. **Proceed only if pass.** If broken, STOP — do not flash on a broken foundation.

## Stop conditions (halt + Slack + write state here)

- Pre-wake path (c) FAILS or gets BLOCKED in a way not resolvable: per Chris,
  push through the risk — but if genuinely blocked, Slack specifics and leave the
  satellite on last-known-good firmware. Do not ship a boot-looping device.
- Firmware build fails twice consecutively, or COM6 flash fails twice.
- Bridge won't restart cleanly.
- Self-verify gate fails.
- Any change would touch v22 gating or the verbosity prompt.
- 2 consecutive failed attempts on the same sub-problem.
- 12 h hard stop. At 11 h, stop starting new work.

## Validation criteria (all must be true, evidence pasted here)

1. A wake event carries a real `probability` value (Option B works).
2. A session has `audio_path` AND the clip contains genuine PRE-WAKE audio
   (verify samples exist BEFORE the wake timestamp — not just post-connection).
3. `/events` renders live with the play button reaching a real clip.
4. STT verdict + `would_suppress` flag in the record (observe-only, no suppression).
5. `time:` sync producing sane satellite timestamps.
6. One autotest-triggered session shows the full per-step timeline.

Evidence = actual jsonl lines + audio file path with pre-wake sample count.
"Should work" is not evidence.

---

## Implementation status

### Pre-step
- Daemon kill: PENDING
- COM6 confirm: PENDING

### Bridge layer
- `bridge/event_log.py`: PENDING
- `POST /satellite-event`: PENDING
- `/events` SSE viewer: PENDING
- Instrumentation calls in `main.py`: PENDING
- STT speech-gate observer: PENDING

### Self-verify gate
- Result: PENDING

### Firmware
- `time:` SNTP: PENDING
- `aria_bridge::emit_event_`: PENDING
- YAML wires: PENDING
- MWW Option B patch: PENDING
- Pre-wake audio (path a/b/c): PENDING
- Build + COM6 flash: PENDING

### Validation
- All 6 criteria: PENDING

---

## Bridge-side self-verify result

*(to be filled in after self-verify run)*

## STT timing finding (transcription arrival vs response.created)

*(to be measured from self-verify event log)*

## Pre-wake path finding (a/b/c + cost)

*(to be filled in as investigated)*

## Validation evidence

### 1. Wake event probability
*(jsonl line here)*

### 2. Audio path + pre-wake samples
*(file path + sample count here)*

### 3. /events live with play button
*(screenshot path or working URL here)*

### 4. STT verdict + would_suppress
*(jsonl line here)*

### 5. time: sync sane timestamps
*(jsonl line with synced ts here)*

### 6. Autotest session full per-step timeline
*(sequence of jsonl lines here)*

---

## Divergences (running log — reality vs original plan)

*(append entries as they happen, with reason)*

---

## Build Complete summary

*(filled in at end)*
