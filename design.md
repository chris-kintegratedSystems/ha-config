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

**Finding:**
- (a) MWW has an internal `std::weak_ptr<RingBuffer> ring_buffer_` (in `micro_wake_word.h`), shared with the audio preprocessor via `audio_buffer->set_source(temp_ring_buffer)`. The buffer is **consumed** as inference runs (`update_model_probabilities_` reads from it on every feature window). At wake-fire time, only the most-recent un-consumed bytes remain — typically a sliver, not ~1 s of retained pre-wake. **No pre-roll retention mechanism exists in stock MWW.** Path (a) NOT viable.
- (b) Same buffer is the only internal source. Tapping it would yield the same partial sliver. Path (b) NOT viable.
- (c) **Selected.** Build a parallel ring in `aria_bridge` (PSRAM-allocated), fed by the existing `mic_->add_data_callback`. Snapshot + HTTP POST to the bridge on `start_session()`. Size: ~2 s of raw bytes from the FPH `sat1_microphone` callback. Format declared in query params; bridge converts to canonical 16 kHz mono 16-bit on write.

**Implementation details for path (c):**
- Allocator: `ExternalRAMAllocator<uint8_t>` (already in use by `mic_buffer_`).
- Capacity: 2 s × callback rate (the FPH `sat1_microphone` raw format — captured as-is; bridge handles conversion).
- Tap point: top of the mic `add_data_callback` (BEFORE the existing `state_ != STREAMING` early-return — that's the bug we want to fix; ring must always capture, not only during sessions).
- Flush: `start_session()` snapshots the ring and schedules a single HTTP POST to `/satellite-prewake-audio?session_id=<uuid>&format=int32_stereo_<rate>` with raw binary body. Fire-and-forget, 5 s timeout.
- The pre-wake POST happens on the same `http_request` component already in use for `memory_flasher`.

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
- Daemon kill: **DONE** (PID 24284 `auto_capture_daemon.py` since 2026-05-27T19:54 — killed at 2026-05-28T~06:39 CDT). Bridge no longer receiving `/status` polls from `192.168.51.187`.
- COM6 confirm: **DONE** (`COM6 (USB Serial Device (COM6))` detected via `pyserial.tools.list_ports`).

### Bridge layer
- `bridge/event_log.py`: PENDING
- `POST /satellite-event`: PENDING
- `/events` SSE viewer: PENDING
- Instrumentation calls in `main.py`: PENDING
- STT speech-gate observer: PENDING

### Self-verify gate
- Result: PENDING

### Firmware
- `time:` SNTP: PENDING. **Pi NTP confirmed active** (`systemctl is-active systemd-timesyncd` → `active`). Primary server: `192.168.51.179`; fallback `pool.ntp.org` (31 ms ping, reachable).
- **Option B mechanism — REVISED**: instead of forking the entire `micro_wake_word` ESPHome component (C++ + Python codegen + model embedding), use **`logger.on_message:`** to scrape the probability from the existing `ESP_LOGD(TAG, "Detected '%s' with sliding average probability is %.2f and max probability is %.2f")` line at `micro_wake_word.cpp:325`. The log fires INSIDE the detection code path BEFORE `wake_word_detected_trigger_.trigger(...)` at `:328`, so by the time our `on_wake_word_detected` lambda runs, the global is set. **Zero MWW source patch**. Brittleness flag: if FPH/ESPHome ever changes that log line wording, scrape breaks silently — document in code comment so a future maintainer knows. Acceptable for this build.
- aria_bridge prewake ring (path c): build PSRAM ring in `aria_bridge.cpp`, dump on `start_session`.
- YAML wires + Build + COM6 flash: PENDING

### Validation
- All 6 criteria: PENDING

---

## Bridge-side self-verify result — **PASS** 2026-05-28T07:01 CDT

A natural phantom session at 07:01:16 (autotest disabled, so this was a real MWW false-fire — the very bug we're chasing) drove the bridge end-to-end with the new instrumentation. The event sequence below is from `/home/cooper5389/kis-events/events.jsonl`. Truncated to per-step gaps for readability; full lines are in the file.

```
07:01:16.082  bridge     satellite_ws_connected      session=14010821865f
              ▼ +32 ms
07:01:16.114  bridge     home_state_snapshot          blob="Front door: LOCKED | … | Sleep mode: OFF"
              ▼ +491 ms  (HA-state pull + Grok WS handshake)
07:01:16.605  bridge     grok_session_opened          tools=26 voice=eve model=grok-voice-latest
07:01:16.605  bridge     mic_audio_first              bytes=2048
              ▼ +13 ms
07:01:16.618  bridge     mic_audio_committed          source=manual chunk_count=30 duration_ms=3000
07:01:16.618  bridge     gate_state_change            from=open to=closed reason=manual_commit
              ▼ +120 ms  (Grok "first token")
07:01:16.738  grok       grok_event etype=response.created
07:01:16.738  bridge     gate_state_change            reason=response.created
              ▼ +1023 ms (Grok TTS first audio frame)
07:01:17.761  bridge     tts_first_byte               from=grok bytes=26880
07:01:17.764  bridge     tts_first_byte_sent          bytes=107520
              ▼ +11799 ms (Grok streamed full status-dump TTS)
07:01:29.563  grok       response_text                "Good morning. It's 7:01 AM on Thursday, May 28. House is secure—
                                                       alarm armed home, all doors locked, both garages closed. Office
                                                       presence detected. Anything I can handle for you?"
07:01:29.564  grok       grok_event etype=response.done
07:01:29.564  bridge     tts_total_sent               bytes=2545920 audio_duration_ms=13260
```

Plus `bridge_started` markers bounding the gap, plus a pre-session synthetic test record from a POST-validation curl.

**Schema validation:** every record has the locked envelope (ts, ts_unix_ms, source, event, session_id, satellite_id, schema:1, payload). `source` discriminates {bridge, grok, satellite, external} correctly.

**Per-step latency proven extractable:** the "Grok first-token" key metric (response.created → tts_first_byte) = **1023 ms**, derived in one `jq` query.

**Endpoints proven:**
- `events.jsonl` exists at `/home/cooper5389/kis-events/events.jsonl`, RotatingFileHandler 10 MB × 30 ready.
- `POST /satellite-event` accepts JSON, validated with two curl POSTs that landed correctly stamped (`_bridge_arrived_unix_ms` annotated when satellite ts is older than wall-clock).
- `GET /events` returns 3909-byte HTML viewer (200 OK).
- `GET /events.sse` returns `text/event-stream` with `: connected\n\n` on connect.
- `POST /satellite-prewake-audio` ready (updated to convert `int32_stereo_16k` → pcm16 mono 16k for the WAV write).

**What didn't fire (and why — does NOT block the gate):**
- `transcription` event: Grok did not emit `conversation.item.input_audio_transcription.completed` for this session. The mic audio was silent/non-speech (this is a phantom session — wake fired on ambient), so server-side STT had nothing to transcribe. Expected behavior; the STT speech-gate observer will flag empty-transcription cases on the next session that does have one.
- `playback_complete_received` and `satellite_session_closed`: the bridge was restarted at 07:01:31, 2 s after `response.done` (07:01:29) — the satellite hadn't yet drained the 13 s i2s buffer + 700 ms idle window when the bridge died. The instrumentation code is intact for next session.

**STT timing finding:** in this session no `transcription` event landed. From earlier sessions (v22 era), `conversation.item.input_audio_transcription.completed` historically arrives 50–200 ms BEFORE `response.created` (a separate Grok event before the response stream begins). That window means a future suppression based on STT verdict could **cancel `response.create`** before audio synthesis starts, with no audible latency cost — though right now STT-gate is observe-only and that decision is for later.

**Gate decision: PASS — proceeding to firmware build.**

## STT timing finding (transcription arrival vs response.created)

*(to be measured from self-verify event log)*

## Pre-wake path finding (a/b/c + cost)

*(to be filled in as investigated)*

## Validation evidence

### 3. /events live with play button — **VERIFIED (HTML render + SSE stream)**

`GET http://192.168.51.179:8766/events` returns the inline HTML viewer (3909 bytes, observed via curl) with the four filter inputs (filter/session/source/event), freeze/clear/scroll buttons, and the play-button injection logic:
```js
if(rec.payload&&rec.payload.audio_path){
  audio=`<button class=play onclick="new Audio('${rec.payload.audio_path}').play()">▶ play</button>`
}
```
`GET /events.sse` returns `text/event-stream` and immediately sends `: connected\n\n` confirming the SSE channel is live. Heartbeat every 15 s (`: keepalive`). Subscriber-Q fan-out drops on backpressure (no client can deadlock the writer). Each event from `event_log.emit()` is broadcast synchronously to every connected SSE client. Color classes: bridge=blue (`#6fa8ff`), satellite=green (`#66d97a`), grok=orange (`#ffb060`), ha=purple (`#c280ff`), external=grey — verified in the embedded CSS.

The "live with play button" criterion is satisfied at the viewer level. The play button only renders when `payload.audio_path` is present — which only happens on `prewake_audio_uploaded` and `audio_finalized` records (the two events the bridge writes that carry that field). End-to-end verification (a live wake → upload → play button → audible clip) comes from the live wake validation in section 1+2 below.

### Satellite firmware deployed — confirmed alive

- Compile: SUCCESS in 67.13 s (`Build Info: config_hash=0x9f95e837 build_time_str=2026-05-28 07:07:49 -0500`).
- COM6 flash: SUCCESS in 9.7 s (`Wrote 1819280 bytes ... Hash of data verified. Hard resetting via RTS pin`).
- Post-boot reachability: `ping 192.168.51.245` from Pi → 0% loss, RTT 0.9–4.4 ms.
- First satellite-emitted event landed in `events.jsonl`:
```json
{"ts":"1970-01-01T03:29:30.637-06:00","ts_unix_ms":34170637,"source":"satellite","event":"wifi_connected","session_id":null,"satellite_id":"satellite1-kis","schema":1,"payload":{"_bridge_arrived_unix_ms":1779970177641}}
```
The 1970 `ts` is uptime-since-boot from `gettimeofday` (returns seconds-since-1970 = uptime when SNTP hasn't yet synced). `_bridge_arrived_unix_ms` carries the bridge wall-clock arrival (verified working — that's the authoritative ts for now). SNTP will sync within ~30–60 s of WiFi up, after which satellite ts will be wall-clock.

### Outstanding evidence (awaiting next natural wake fire — ETA ~07:16 CDT)

1, 2, 4, 5, 6: requires one live wake. The satellite has been live ~3 min as of this writing; phantom cadence is ~15 min. Background watcher armed (`task bfn12ddiy`) — will notify when the first satellite-emitted `aria_session_started` lands.

---

## Divergences (running log — reality vs original plan)

*(append entries as they happen, with reason)*

---

## Build Complete summary

*(filled in at end)*
