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
light.patio_string_lights
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
| Doorbell | `camera.doorbell` | Vivint DBC300. Use `camera_view: auto` (not live). DBC300 has RTSP at `rtsp://user:PASS@IP:PORT/Video-00` but needs go2rtc to work in HA |
| Izzy Camera | `camera.izzy_camera` | Nest camera. Use `camera_view: auto`. Rate-limit prone. |
| Living Room | `camera.living_room_camera` | Nest camera. Use `camera_view: auto`. |
| Doorbell Motion | `binary_sensor.doorbell_motion` | |

**Camera rules:**
- Always use `camera_view: auto` in dashboard cards to avoid rate-limiting
- go2rtc NOT currently set up — on backlog
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

## ⚠️ Known Issues / Backlog

| Issue | Status | Notes |
|-------|--------|-------|
| Node 25 Z-Wave | Open | One-way communication failure — 0 return pings |
| Garage TriSensor packet drops | Monitor | ~50% RX drop rate — consider Z-Wave repeater |
| Doorbell live stream | Backlog | go2rtc not set up; using snapshot mode for now |
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
