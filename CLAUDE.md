# Home Assistant — Claude Code Project
# Irving, TX | Raspberry Pi 5 | Docker Stack

---

## 🏠 Live Instance

| Item | Value |
|------|-------|
| URL | http://192.168.51.179:8123 |
| SSH | `ssh cooper5389@192.168.51.179` |
| Username | `cooper5389` |
| OS | Debian Bookworm (Raspberry Pi 5) |
| Stack | Docker / Docker Compose / Portainer |
| Portainer | http://192.168.51.179:9000 |
| Z-Wave JS UI | http://192.168.51.179:8091 |

---

## 📁 File & Directory Structure (on Pi)

```
/home/cooper5389/
├── homeassistant/
│   ├── docker-compose.yml          ← HA container definition
│   └── config/                     ← ALL HA config lives here
│       ├── configuration.yaml
│       ├── automations.yaml
│       ├── scripts.yaml
│       ├── scenes.yaml
│       ├── lovelace.yaml           ← Main dashboard (manually managed)
│       └── secrets.yaml            ← NEVER commit this to git
├── zwavejs/
│   ├── docker-compose.yml
│   └── store/
```

### Docker Containers
| Container | Purpose | Update Command |
|-----------|---------|----------------|
| `homeassistant` | Home Assistant (host network mode) | `cd ~/homeassistant && sudo docker compose pull && sudo docker compose up -d` |
| `zwave-js-ui` | Z-Wave JS UI | `cd ~/zwavejs && sudo docker compose pull && sudo docker compose up -d` |
| `tender_lederberg` | Portainer | Update via http://192.168.51.179:9000 UI |

---

## 🚀 Deployment Workflow

### NEVER deploy directly — always test first.

### Standard config file deploy (from Windows PC):
```powershell
scp ~/Downloads/lovelace.yaml cooper5389@192.168.51.179:/home/cooper5389/homeassistant/config/lovelace.yaml
```

### After deploying lovelace.yaml:
- Hard refresh browser: `Ctrl+Shift+R`
- No HA restart needed for Lovelace changes

### After deploying configuration.yaml / automations.yaml:
```bash
# SSH in first, then:
sudo docker exec homeassistant ha core check   # validate config
sudo docker exec homeassistant ha core restart  # restart HA
```

### Maintenance (run in order):
```bash
sudo apt update && sudo apt upgrade -y
sudo apt autoremove -y && sudo apt autoclean
cd ~/homeassistant && sudo docker compose pull && sudo docker compose up -d
cd ~/zwavejs && sudo docker compose pull && sudo docker compose up -d
sudo docker image prune -f
sudo reboot   # only if kernel was updated
```

---

## 🔑 Git / GitHub Workflow

- **Repo:** Private GitHub repo (ha-config)
- **Never commit:** `secrets.yaml`, `.storage/`, `home-assistant_v2.db`, `backups/`
- **Always commit:** `lovelace.yaml`, `automations.yaml`, `configuration.yaml`, `scripts.yaml`, `CLAUDE.md`
- **Deploy via git on Pi:**
  ```bash
  cd ~/homeassistant/config && git pull
  ```

---

## 🔧 Integrations & Key Entities

### 🔒 Locks
| Name | Entity ID |
|------|-----------|
| Front Door | `lock.front_door_lock` |
| Back Door | `lock.back_door_lock` |
| Gemelli Door | `lock.gemelli_door` (verify exact ID) |

### 🚗 Garage Doors (ratgdo)
| Name | Entity ID | Notes |
|------|-----------|-------|
| Left Garage Door | `cover.ratgdov25i_1746c3_door` | Opening: 11.1s, Closing: 13.2s |
| Right Garage Door | `cover.ratgdov25i_1746b4_door` | Opening: 11.2s, Closing: 12.4s |
| Left Garage Light | `light.ratgdov25i_1746c3_light` | |
| Right Garage Light | `light.ratgdov25i_1746b4_light` | |
| Left Obstruction | `binary_sensor.ratgdov25i_1746c3_obstruction` | |
| Right Obstruction | `binary_sensor.ratgdov25i_1746b4_obstruction` | |
| Left Motion | `binary_sensor.ratgdov25i_1746c3_motion` | |
| Right Motion | `binary_sensor.ratgdov25i_1746b4_motion` | |

### 🌡️ Thermostats
| Name | Entity ID | Notes |
|------|-----------|-------|
| Living Room | `climate.daikin` | Modes: auto, heat, cool, off. Use `Math.round()` for temp display. |
| Gemelli | `climate.gemelli` | |
| Master | `climate.master` | |
| Upstairs | `climate.upstairs` | Ecobee |

### 🔐 Alarm
| Entity | Notes |
|--------|-------|
| `alarm_control_panel.kuprycz_home` | States: disarmed, armed_home, armed_away, arming, pending, triggered |

Alarm integration: **Vivint** (HACS: natekspencer/ha-vivint)
- Known issue: 520 errors are intermittent Vivint cloud errors, not a code bug
- Plan: Keep Vivint for arm/disarm only, migrating all devices to HA-native

### 📡 Z-Wave
| Device | Entity ID | Node | Notes |
|--------|-----------|------|-------|
| AeoTec TriSensor (Garage) | `binary_sensor.aeotec_trisensor_garage_motion_detection` | Node 35 | Wake-up interval: 3600s, Untrigger: 60s |
| Node 25 | Unknown | 25 | One-way comm failure — 0/10 return pings, needs investigation |

**Z-Wave tips:**
- Battery devices: press button once to wake and sync config changes
- After battery pull: must re-include (exclude → include), re-set untrigger + wake interval
- 50% packet drop on garage sensor — consider Z-Wave repeater in/near garage

### 💡 Lights
```
light.countertop_lights
light.kitchen_chandelier
light.kitchen_ceilings_lights
light.kitchen_island_light
light.living_room_ceiling
light.living_room_lamp_2
light.garage_light
light.outdoor_switch_2  # friendly name: "Patio String Lights"
light.front_porch_lights
light.front_walkway_lights
light.upper_outdoor_lights
light.left_outdoor_patio_lights
light.center_outdoor_patio
light.benjamins_hatch_light
light.bens_light
```

### 📹 Cameras
| Name | Entity ID | Notes |
|------|-----------|-------|
| Doorbell | `camera.doorbell` | Vivint DBC300. Use `camera_view: auto` (not live). DBC300 RTSP is fronted by the go2rtc container (stream name `doorbell`). |
| Nest Cam 1 | `camera.nest_cam_1` | Nest camera — physically in Living Room. Priority key: `living_room`. Use `camera_view: auto`. Rate-limit prone. |
| Nest Cam 2 | `camera.nest_cam_2` | Nest camera — physically in Ben's Room. Priority key: `bens_room`. Use `camera_view: auto`. |
| Nanit Benjamin | `camera.nanit_benjamin` | Nanit baby monitor via local RTMP restream. ffmpeg platform. See "Nanit integration" below. |
| Nanit Travel | `camera.nanit_travel` | Portable Nanit unit. Same restream container. |
| Doorbell Motion | `binary_sensor.doorbell_motion` | |

**Camera rules:**
- Always use `camera_view: auto` in dashboard cards to avoid rate-limiting
- go2rtc runs as a standalone container on the Pi:
  - Image: `alexxit/go2rtc:latest` (running version **v1.9.14** as of
    2026-04-20, linux/arm64)
  - Host networking mode
  - Config: `/home/cooper5389/go2rtc/config/go2rtc.yaml`
  - Ports: API `:1984`, RTSP `:8554`, WebRTC `:8555/tcp`
  - Only the `doorbell` stream is configured (Vivint DBC300 RTSP, plus a
    `camera.doorbell` variant for ffmpeg-sourced audio). Nest and Nanit
    cameras are **not** in go2rtc — they flow through their native HA
    integrations (Nest: google_nest_sdm, Nanit: ffmpeg platform against
    the local indiefan/nanit RTMP restream). Adding them + wiring
    `custom:webrtc-camera` for real two-way audio is a future scope.
- Nest cameras: WebRTC/RTSP warnings are a Google API limitation, not fixable
- Do NOT open camera dashboard on multiple devices simultaneously

### 🌿 Other Integrations
| Integration | Notes |
|-------------|-------|
| Rachio | Irrigation system |
| Ecobee | Upstairs thermostat (`climate.upstairs`) |
| Google Nest | Cameras (natekspencer integration, go2rtc via HA internals) |
| browser_mod | Used for popup cards on dashboard |
| HACS | Frontend: button-card, mushroom, card_mod, bubble-card, kiosk-mode, clock-weather-card, browser_mod |

### 👤 Presence / People
| Person | Entity | Notes |
|--------|--------|-------|
| Chris | `person.chris` | `sensor.chris_distance_from_home` |
| Claire | `person.claire` | `sensor.claire_distance_from_home` — requires HA Companion app on iPhone, Location set to Always, Background App Refresh on |
| Benjamin | Family member | No tracking entity |

---

## 📱 Dashboards

### Mobile + Tablet Dashboards — managed in `ha-dashboard` repo
Storage-mode dashboards `dashboard-mobilev1` and `dashboard-tabletv1`
live as JSON in the `ha-dashboard` sibling repo and deploy to
`/config/.storage/lovelace.dashboard_mobilev1` and
`/config/.storage/lovelace.dashboard_tabletv1` (no `.json` extension,
`root:root` 644, requires `sudo docker restart homeassistant` after
copy). See `C:\Projects\kintegrated\customers\ha-dashboard\CLAUDE.md`
→ "Critical Patterns → dashboard target path". Do NOT deploy these
JSONs to `/config/www/` — that is a dead letter for dashboard
configs; HA only reads dashboard Lovelace configs from `.storage/`.

### Main Dashboard (`lovelace.yaml`)
- **File:** `/home/cooper5389/homeassistant/config/lovelace.yaml`
- **Card standard:** `custom:button-card` with `state:` blocks for dynamic color/icon — this is the confirmed working approach
- **Card height standard:** `80px` for all control cards (locks, garage, climate, presence)
- **Kiosk mode:** Configured at top of lovelace.yaml — hides header/sidebar on mobile AND tablet

```yaml
kiosk_mode:
  hide_header: true
  hide_sidebar: true
  mobile_settings:
    hide_header: true
    hide_sidebar: true
  tablet_settings:
    hide_header: true
    hide_sidebar: true
```

### Dashboard Sections (in order)
1. Clock & Weather (`custom:clock-weather-card`, entity: `weather.forecast_home`)
2. Home Alarm (`alarm_control_panel.kuprycz_home`)
3. Door Locks (horizontal-stack of 3 button-cards)
4. Garage Doors (horizontal-stack of 2 button-cards with ratgdo entities)
5. Climate (2x2 grid of button-cards — Daikin, Gemelli, Master, Upstairs)
6. Lights (entities card, `show_header_toggle: false`)
7. Cameras (3x picture-entity cards with browser_mod popups)
8. Presence (horizontal-stack, Chris + Claire mushroom-template-cards)

### Devices
| Device | Dashboard | Notes |
|--------|-----------|-------|
| Samsung Galaxy Tab S9+ | Wall-mounted kiosk | Fully Kiosk Browser, main lovelace view |
| iPhone (Chris) | Companion app | Mobile dashboard, default view |
| iPad | Companion app | Tablet — kiosk_mode tablet_settings required |

---

## Camera Follow Code

Stateful priority camera lock with 60-second trailing hold for the
mobilev1 priority-display zone. Replaces the previous stateless
`*_motion_sticky` binary sensors + if/elif cascade.

### How it works
When a camera detects a person (`*_person_occupancy` → on), it becomes
the locked camera. The lock holds for as long as person_occupancy is
active PLUS a 60-second trailing window after it clears. During the
trailing window, another camera can preempt if the locked camera is
quiet. The doorbell is a hard override — it always wins regardless of
what's currently locked.

### Files involved
| File | What |
|------|------|
| `configuration.yaml` | `input_text.priority_camera_lock`, `timer.priority_camera_release`, `sensor.priority_camera` template |
| `automations.yaml` | 7 automations aliased "Camera Follow Code — <step>" |

### Priority order
doorbell > living_room > bens_room > nanit_benjamin > nanit_travel

### Doorbell hard-override rule
Doorbell always preempts any other locked camera. No other camera can
preempt doorbell. Doorbell's trailing timer is managed separately from
the general camera clear logic.

### How to disable / replace
1. Delete all 7 "Camera Follow Code" automations from `automations.yaml`
2. Restore the `sensor.priority_camera` template to its original
   if/elif cascade reading `*_person_occupancy` + `*_motion_sticky`
3. Recreate the 5 `*_motion_sticky` template binary_sensors in
   `configuration.yaml` (delay_off: 00:00:05)
4. Update dashboard conditional cards back to
   `binary_sensor.*_motion_sticky == 'on'`
5. Remove `input_text.priority_camera_lock` and
   `timer.priority_camera_release` helpers

---

## ⚠️ Known Issues / Backlog

| Issue | Status | Notes |
|-------|--------|-------|
| Node 25 Z-Wave | Open | One-way communication failure — 0 return pings |
| Garage TriSensor packet drops | Monitor | ~50% RX drop rate — consider Z-Wave repeater |
| Doorbell two-way audio | Backlog | go2rtc container running with doorbell stream, but `custom:webrtc-camera` HACS card not installed and Vivint DBC300 backchannel support unverified |
| Duplicate `logger:` in configuration.yaml | Fix needed | Lines 11 and 85 — YAML only uses last one |
| `exclude` option deprecated at line 113 | Minor | In `recorder:` or `history:` config |
| Benjamin's TV Chromecast removed | Done | Deleted device to stop log spam |
| Vivint 520 errors | Monitor | Intermittent Vivint cloud errors, not code bug |
| Claire location not updating | Monitor | Companion app background location; check iOS settings |

---

## 🛠️ YAML / Dev Rules

1. **Test before deploying to production** — use test Docker HA on port 8124 if needed
2. **Backup before changes:** `ssh cooper5389@192.168.51.179 "tar -czf ~/ha-backup-$(date +%Y%m%d-%H%M%S).tar.gz ~/homeassistant/config"`
3. **Lovelace:** Use `custom:button-card` with native `state:` blocks for dynamic styling. Avoid Bubble Card and Mushroom for color-dynamic cards (shadow DOM issues)
4. **Secrets:** All API keys, passwords, PINs go in `secrets.yaml` referenced as `!secret key_name`. Never hardcode.
5. **YAML validation before restart:** `sudo docker exec homeassistant ha core check`
6. **Deploy lovelace:** `scp` from Windows → hard refresh browser. No restart needed.
7. **Automations:** Use trigger IDs when branching behavior. Always add conditions to prevent re-triggering when state is already correct.

---

## 🚨 Critical Patterns

### configuration.yaml — extra_module_url preservation
Every deploy that touches configuration.yaml MUST verify that the
frontend.extra_module_url block is present and includes kis-nav.js
with the current cache-bust version:

```yaml
frontend:
  themes: !include_dir_merge_named themes
  extra_module_url:
    - /local/mobile_v1/kis-nav.js?v=<CURRENT_VERSION>
```

If this block is missing, kis-nav.js will not load and the header bar,
bottom nav bar, notification badges, and mini-player will all disappear.
This is invisible to Playwright QA since anonymous sessions never load
custom JS resources.

**Root cause:** Phase 4 deploy (April 2026) — ha-config branch modified
configuration.yaml without the extra_module_url block. SCP to Pi
overwrote the working config and stripped the kis-nav.js loader.

**Prevention checklist for any configuration.yaml change:**
1. Before SCP: grep for `extra_module_url` in the local file
2. Before SCP: grep for `kis-nav` in the local file
3. If either is missing, STOP — do not deploy
4. After docker restart: verify on real device that header + nav appear

### Playwright limitations
Playwright QA (qa-screenshot.js) runs anonymous Chromium sessions that
do NOT load kis-nav.js, HA themes, or authenticated entity states.

Playwright CANNOT verify:
- Header bar (time, weather, presence, alarm)
- Bottom nav bar (6 tabs, badges)
- Mini-player
- Notification badge counts
- Day/night theme rendering
- Entity-driven conditional cards

After every deploy, verify these on real hardware:
- Tab S9 via Fully Kiosk hard refresh
- iPhone via HA Companion App hard refresh
- Check day AND night mode on both devices

---

## Deploy Discipline — Regression Diagnosis

When a bug "appears" coinciding with a deploy, the default hypothesis
is "the deploy caused it." Disproving that requires a positive
mechanism for prior invisibility — not just "must have been hidden
somehow."

If the mechanism can't be verified, log the uncertainty in the PR
description and proceed only if BOTH:

(a) The fix is mechanically correct independent of cause — i.e. the
    same change would be correct whether or not the deploy caused
    the regression
(b) The fix is on its own revertable PR — not folded into the deploy
    that may have caused the regression

Never merge with the explanation "pre-existing bug, no idea why it
just appeared" treated as established fact. Either name a specific
mechanism for prior invisibility (commit history, conditional
visibility change, entity load order) or explicitly log the
uncertainty.

Example: Camera Follow Code deploy (PR #38, ha-dashboard) coincided
with ButtonCardJSTemplateError on Gemelli lock card. Initial diagnosis
was "pre-existing wrong entity ID." Chris confirmed he had not seen
the error before. Mechanism for prior invisibility was not verified.
Fix shipped on isolated PR #39 because (a) lock.gemelli_door_lock →
lock.gemelli_door was mechanically correct regardless of cause, and
(b) PR #39 was independent of PR #38 and revertable. PR description
explicitly logged the unverified causal story.

---

## 🍼 Nanit Integration

Nanit cameras stream into HA via a local RTMP restream container
(`indiefan/nanit`). The container authenticates against Nanit cloud
using `NANIT_EMAIL` + `NANIT_PASSWORD` env vars, then mirrors each
baby's feed over RTMP on the Pi so HA's `ffmpeg` camera platform can
consume it without hitting Nanit's API for every view.

### Docker container

| Field | Value |
|-------|-------|
| Image | `indiefan/nanit` (unpinned — last pulled 2026-04-20) |
| Compose source | `C:\Projects\kintegrated\nanit\docker-compose.yaml` (local); `/home/cooper5389/nanit/docker-compose.yaml` (Pi) |
| Container name | `nanit` |
| Port | `1935/tcp` (RTMP) |
| Volume | **bind mount** `/home/cooper5389/nanit/data:/data` — persists session/refresh state |
| Restart policy | `unless-stopped` |
| Env — `NANIT_RTMP_ADDR` | `192.168.51.179:1935` |
| Env — `NANIT_EMAIL` | set in compose file (account: `chris.kuprycz@gmail.com`) |
| Env — `NANIT_PASSWORD` | **currently plain-text in compose file — move to a .env / secrets pattern** (flagged 2026-04-20) |
| Env — `NANIT_LOG_LEVEL` | `trace` |

> ⚠️ **Security follow-up:** `NANIT_PASSWORD` is in plain text inside
> `/home/cooper5389/nanit/docker-compose.yaml`. Convert to a
> docker-compose `.env` file or a `secrets:` mount before the next
> touch on this container. Do NOT commit the password to the
> `ha-config` repo — the `nanit/docker-compose.yaml` lives in the
> `kintegrated/nanit` repo tree; use `env_file:` referencing a
> `.env` that is `.gitignore`d.

### Auth flow

The indiefan image authenticates from env vars on every container
start. On first launch (or after a password change / forced logout),
Nanit emails a 2FA code; the container consumes it automatically if
the Nanit API returns the expected challenge shape. Session state
persists in the bind-mounted `/home/cooper5389/nanit/data` directory,
so normal restarts reuse the refresh token and do not re-prompt.

### Baby UIDs

Baby UIDs print to the container log on first authenticated run and
never change for a given camera:

```bash
docker logs nanit | grep -i 'baby_uid'
```

Known UIDs (stored in the live `secrets.yaml` — the full RTMP URL
is the secret, not just the UID, because HA YAML does not
interpolate `!secret` inside strings):

```yaml
nanit_benjamin_rtmp: "rtmp://192.168.51.179:1935/local/<benjamin_baby_uid>"
nanit_travel_rtmp: "rtmp://192.168.51.179:1935/local/<travel_baby_uid>"
```

`configuration.yaml` references these via `!secret`.

### Stream URLs

| Entity | URL |
|--------|-----|
| `camera.nanit_benjamin` | `rtmp://192.168.51.179:1935/local/<benjamin_baby_uid>` |
| `camera.nanit_travel` | `rtmp://192.168.51.179:1935/local/<travel_baby_uid>` |

### Known limitations

- **Motion events not exposed.** The fork publishes motion, sound,
  cry, standing, and temperature via MQTT auto-discovery, but HA on
  this Pi has no MQTT broker (mosquitto) configured as of
  2026-04-20. Until a broker is added and `NANIT_MQTT_ENABLED=true`
  in the compose file, Nanit cameras cannot participate in the Home
  dashboard's motion-triggered camera zone. Enabling this is a
  separate phase (add mosquitto container → enable MQTT env vars
  → flip conditional-visibility cards in `dashboard_mobilev1.json`
  home section to reference `binary_sensor.nanit_*_motion`).
- **Audio is not supported.** The RTMP restream mirrors video only.
  Two-way talk/listen (which Nanit's first-party app offers) is not
  available through this integration.
- **Single-subscriber preference.** Nanit's local stream is optimized
  for one viewer at a time; opening the feed on multiple devices
  simultaneously can trigger stream failover to the cloud source.

---

## 🔄 Common Claude Code Tasks

```bash
# SSH to Pi
ssh cooper5389@192.168.51.179

# Check HA logs (last 100 lines)
sudo docker logs homeassistant --tail 100

# View HA error log
sudo docker exec homeassistant cat /config/home-assistant.log | tail -50

# Validate config before restart
sudo docker exec homeassistant ha core check

# Restart HA
sudo docker exec homeassistant ha core restart

# Pull current lovelace to local for editing
scp cooper5389@192.168.51.179:/home/cooper5389/homeassistant/config/lovelace.yaml ./

# Push lovelace back
scp ./lovelace.yaml cooper5389@192.168.51.179:/home/cooper5389/homeassistant/config/lovelace.yaml

# Pull automations
scp cooper5389@192.168.51.179:/home/cooper5389/homeassistant/config/automations.yaml ./

# Watch live HA logs
sudo docker logs -f homeassistant

# Check all container health
sudo docker ps

# Clean old images
sudo docker image prune -f
```

---

## WebRTC Camera Stream — Investigation Notes (May 2026)

Status: WebRTC enabled via Frigate integration option
`enable_webrtc=True`. Architecture is correct and rate-limit-safe.

### Observed Behavior
- Stationary clients (wall kiosk Tab A9+ at .150): stable WebRTC
  connection, smooth playback
- Mobile iPad held in hand while walking through house: choppy
  with 5-10 second freeze cycles
- Laptop on Wi-Fi (.183): WebRTC session teardown/rebuild every
  12-23 seconds, likely Chromium responding to hardware decoder
  fallback
- Nanit Travel camera: smooth on all clients (control case —
  1-second keyframe interval, lower resolution, lower bitrate)

### Root Cause
Wi-Fi AP handoffs during mobile use. Six WiFi 5 APs in the home
without 802.11r/k/v fast roaming. Each handoff causes 1-3 seconds
of network disruption, during which WebRTC over UDP loses packets.
After disruption, video freezes until next I-frame arrives.

Nest cameras have 2-second keyframe interval → up to 2s freeze
recovery → ~5-10s total visible freeze including handoff.
Nanit has 1-second keyframe interval → ~1s freeze recovery →
imperceptible.

### Ruled Out (with evidence)
- Stream-level corruption: bytestream comparison shows clean
  SPS/PPS, zero decode errors on both Nanit and Nest streams
- Network bandwidth: <1% utilization on Pi gigabit eth
- Pi compute: 25% CPU, 8 GB RAM headroom
- Multi-stream contention: laptop on single camera still cycles
- 11-minute Nest SDM reconnect cycle: real (5,504 lifetime
  watchdog events) but doesn't match observed 10-20s pattern
- Hardware decoder rejection on Pi side: Pi isn't decoding for
  browsers, just restreaming

### Fix Paths (queued, not yet implemented)

#### Option A: Reduce Nest keyframe interval to 1 second (Frigate)
Add `input_kf=1` (or equivalent) to Frigate go2rtc restream params
for Nest cameras. Reduces freeze-after-handoff from 2s to 1s.
Doesn't prevent handoff itself, but reduces visible impact.
Cost: minor, possibly small CPU increase on Pi.
Estimated effort: 30 min config + test.

#### Option B: Enable 802.11r/k/v fast BSS transition on Araknis APs
Standard Wi-Fi roaming optimization. Reduces handoff latency from
1-3 seconds down to ~50ms. Addresses root cause for all mobile
WebRTC use, not just cameras.
Cost: zero, infrastructure change in Araknis admin.
Estimated effort: 15-30 min.

Recommendation: ship both. They stack. Option B addresses root
cause, Option A reduces residual impact even with fast roaming.

### Accept-as-is rationale (current state)
WebRTC architecture is correct. Stream quality is correct. Pi
compute is correct. The visible choppiness is a Wi-Fi handoff
limitation that affects mobile clients only. Wall-mounted devices
work as expected.

---

## Camera Live Playback Architecture (May 2026 investigation)

### Investigation summary

Spent multiple sessions diagnosing live camera playback issues
in HA dashboard. Tested across:
- Card types: picture-entity, Advanced Camera Card v7.27.4
- Streaming protocols: HLS, MSE, WebRTC via go2rtc
- Frigate configurations: live_provider variations
- Browser environments: Chrome on wired laptop, multiple sessions

### Per-camera test results (May 2026 baseline)

| Camera | Source Type | Resolution | Server CPU | go2rtc/MSE | go2rtc/WebRTC | HLS |
|--------|------------|------------|------------|------------|---------------|-----|
| Doorbell (Vivint DBC300) | RTSP | 960x960 @ 15fps | 9.2% | Smooth | untested | choppy |
| Nest Cam 1 (Living Room) | Nest SDM | 1080p @ 24fps | 25.0% | catastrophic | smooth-then-cyclic-freeze | choppy |
| Nest Cam 2 (Ben's Room) | Nest SDM | 1080p @ 30fps | 18.1% | (same source as cam_1) | (same as cam_1) | choppy |
| Nanit Benjamin (HW v2.0) | RTMP | 1080p @ 10fps | 12.1% | (same source as travel) | untested | choppy |
| Nanit Travel (HW v1.5) | RTMP | 960p @ 10fps | 6.1% | Smooth, 3sec latency | untested | choppy |

### Root cause finding

Nest cameras through SDM API deliver streams with two compounding
problems:
- Profile-level-id 42001f (H.264 Baseline at 1080p) triggers
  browser hardware decoder rejection on common Chrome/Edge
  builds. Software decoder fallback cannot sustain 1080p25,
  producing freezes.
- Stream renegotiation cycle approximately every 5 minutes
  introduces brief corruption, recreating the freeze pattern
  on a recurring basis.

Frigate processes Nest streams without server-side errors (logs
clean over multiple test sessions). go2rtc delivers streams
faithfully. The constraint is at the source-to-browser pipeline.

RTSP and RTMP sources do not exhibit this behavior. Same Frigate,
same go2rtc, same browser, same network — clean source = smooth
playback.

### Architectural conclusion

Live monitoring smoothness is determined by camera source quality,
not by NVR/dashboard configuration. The right architectural
question for any installation is:

"Are these cameras' stream characteristics compatible with browser
rendering through standard NVR pipelines?"

- RTSP cameras (Reolink, Hikvision, Amcrest, Axis): yes
- Nest cameras through SDM API: no, regardless of NVR
- Vivint cameras through their RTSP integration: yes, with tier
  limitations
- Nanit cameras through indiefan/nanit container: yes

### KIS Cameras Architecture Framework (revised)

Camera SOURCE TIER is independent of NVR/COMPUTE TIER. Both must
be specified for client deliverables.

**Source tiers:**

**Tier A — RTSP-native** (Reolink, Hikvision, Amcrest, Axis)
- Clean H.264 streams, browser-decoder friendly
- Smooth live playback through any standard NVR
- Recommended for any client requiring continuous live monitoring
- Cost: $150-400 per camera depending on features

**Tier B — Smart cameras with proprietary streaming** (Nest, Ring,
Arlo)
- Smart features and event detection excellent
- Live playback through third-party NVRs has documented issues
- Suitable for at-a-glance status awareness and event review
- Live monitoring requires the manufacturer's own app for parity

**Tier C — Alarm system cameras** (Vivint, ADT, etc.)
- Limited stream tiers exposed via integration
- Quality depends on what the proprietary system permits
- Suitable for security integration with the broader alarm system

**Compute tiers (independent of source tier):**

**KIS Cameras Lite** — Pi 5 + Coral USB
- Up to 5 cameras, basic Frigate, HA integration
- Best paired with Tier A cameras
- $250 hardware

**KIS Cameras Standard** — Intel N100 + Coral USB
- Up to 8 cameras, full Frigate features
- Best paired with Tier A cameras, supports mixed
- $400-600 hardware

**KIS Cameras Premium** — Intel N305 + Coral PCIe + redundant storage
- 16+ cameras, full features, dedicated transcoding compute
- Mixed source tiers supported
- $1000+ hardware

### KIS client conversation framework

When clients ask about smooth 24/7 live monitoring:

"Live monitoring smoothness comes from camera selection, not from
more powerful NVR hardware. RTSP cameras through our standard
Pi 5 + Frigate stack deliver smooth multi-camera live to wall
displays and mobile dashboards. If you have existing Nest, Ring,
or similar smart cameras and want to integrate them, we can do
that for the smart features (motion alerts, event recording,
automation triggers) but live viewing of those specific cameras
in the unified dashboard will have limitations because of how
those manufacturers deliver their streams. For live viewing of
those cameras, the manufacturer's app on phone or tablet remains
the smoothest path. We can quote you a hybrid system or a full
RTSP system depending on your priorities."

### Personal home decision (Chris, May 2026)

- mobilev1 retained as production dashboard
- Picture-entity polling for cameras provides at-a-glance status
  awareness, which is the actual daily use case
- Frigate web UI used for event review
- Google Home app used when smooth Nest live viewing specifically
  needed
- No further dashboard live-playback work warranted
- Future incremental upgrade path: add 1-2 RTSP cameras at
  strategic positions (front door supplementing/replacing Vivint,
  or main living area) for unified smooth live at those positions

---

## 📦 .gitignore (for this repo)

```
secrets.yaml
.storage/
home-assistant_v2.db
home-assistant_v2.db-shm
home-assistant_v2.db-wal
backups/
*.log
.cloud/
```
