# KIS Dashboard — Header Card Specification

> This section is the most complex card in the dashboard.
> Read this entire file before attempting to build the header.

---

## What the Header Must Render

```
┌─────────────────────────────────────────┐
│  3:47 PM                                │
│  Friday · April 11                      │
│                                         │
│  ● Disarmed          78°  Partly Cloudy │
│                                         │
│  [C]  [CL]  [B]                         │
└─────────────────────────────────────────┘
```

Six distinct elements, all dynamic, all in one card.

---

## Implementation Approach

Use `custom:button-card` with an `extra_templates` block and a `label` field that renders raw HTML via Jinja2.

This is the ONLY approach that reliably supports:
- Multi-line HTML layout inside a single card
- Inline entity state reads via Jinja2
- CSS styling scoped to the card

**Do NOT use:**
- `type: markdown` — cannot read entity states inline without template sensors
- `type: entities` — wrong layout, cannot be styled as a header
- Multiple stacked cards — creates gaps and misaligned layout

---

## Full YAML

```yaml
type: custom:button-card
show_name: false
show_icon: false
show_label: true
tap_action:
  action: none
styles:
  card:
    - background: "#0f1117"
    - border-radius: 16px
    - padding: 20px 20px 16px 20px
    - margin-bottom: 4px
  label:
    - text-align: left
    - width: 100%
    - white-space: normal
    - font-family: "'DM Sans', system-ui, sans-serif"
label: |
  <div style="width:100%">
    <!-- TIME + DATE -->
    <div style="
      font-family: 'DM Mono', monospace;
      font-size: 36px;
      font-weight: 500;
      color: #f1f5f9;
      line-height: 1;
      margin-bottom: 2px;
    ">
      [[[ return states['sensor.time'] || '--:--' ]]]
    </div>
    <div style="
      font-size: 13px;
      color: #64748b;
      margin-bottom: 14px;
      font-weight: 400;
    ">
      [[[
        const d = new Date();
        const days = ['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday']
        const months = ['January','February','March','April','May','June',
          'July','August','September','October','November','December'];
        return days[d.getDay()] + ' · ' + months[d.getMonth()] + ' ' + d.getDate();
      ]]]
    </div>

    <!-- ALARM + WEATHER ROW -->
    <div style="
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 14px;
    ">
      <!-- Alarm state pill -->
      <div style="
        display: inline-flex;
        align-items: center;
        gap: 6px;
        padding: 4px 12px;
        border-radius: 20px;
        font-size: 12px;
        font-weight: 600;
        letter-spacing: 0.04em;
        text-transform: uppercase;
        background: [[[
          const s = states['alarm_control_panel.kuprycz_home']?.state;
          const colors = {
            disarmed: 'rgba(34,197,94,0.15)',
            armed_away: 'rgba(239,68,68,0.15)',
            armed_home: 'rgba(59,130,246,0.15)',
            arming: 'rgba(245,158,11,0.15)',
            pending: 'rgba(245,158,11,0.15)',
            triggered: 'rgba(239,68,68,0.25)',
          };
          return colors[s] || 'rgba(100,116,139,0.15)';
        ]]];
        color: [[[
          const s = states['alarm_control_panel.kuprycz_home']?.state;
          const colors = {
            disarmed: '#22c55e',
            armed_away: '#ef4444',
            armed_home: '#3b82f6',
            arming: '#f59e0b',
            pending: '#f59e0b',
            triggered: '#ef4444',
          };
          return colors[s] || '#94a3b8';
        ]]];
      ">
        ●&nbsp;
        [[[
          const s = states['alarm_control_panel.kuprycz_home']?.state;
          const labels = {
            disarmed: 'Disarmed',
            armed_away: 'Armed Away',
            armed_home: 'Armed Home',
            arming: 'Arming',
            pending: 'Pending',
            triggered: 'TRIGGERED',
          };
          return labels[s] || 'Unknown';
        ]]]
      </div>

      <!-- Weather -->
      <div style="
        font-size: 13px;
        color: #94a3b8;
        text-align: right;
      ">
        <span style="font-family:'DM Mono',monospace; color:#f1f5f9; font-size:15px;">
          [[[
            const temp = states['weather.forecast_home']?.attributes?.temperature;
            return temp ? Math.round(temp) + '°' : '--°';
          ]]]
        </span>
        &nbsp;Dallas &nbsp;·&nbsp;
        [[[
          const cond = states['weather.forecast_home']?.state;
          const labels = {
            'sunny': 'Sunny',
            'clear-night': 'Clear',
            'partlycloudy': 'Partly Cloudy',
            'cloudy': 'Cloudy',
            'fog': 'Foggy',
            'rainy': 'Rainy',
            'pouring': 'Heavy Rain',
            'snowy': 'Snowy',
            'snowy-rainy': 'Sleet',
            'hail': 'Hail',
            'lightning': 'Lightning',
            'lightning-rainy': 'Storms',
            'windy': 'Windy',
            'windy-variant': 'Windy',
            'exceptional': 'Unusual',
          };
          return labels[cond] || cond || '--';
        ]]]
      </div>
    </div>

    <!-- PERSON AVATARS ROW -->
    <div style="display: flex; gap: 8px; align-items: center;">
      <!-- Chris -->
      <div style="
        width: 36px; height: 36px;
        border-radius: 50%;
        display: flex; align-items: center; justify-content: center;
        font-size: 11px; font-weight: 700; letter-spacing: 0.02em;
        background: [[[
          return states['person.chris']?.state === 'home'
            ? 'rgba(34,197,94,0.2)' : 'rgba(100,116,139,0.15)';
        ]]];
        color: [[[
          return states['person.chris']?.state === 'home' ? '#22c55e' : '#64748b';
        ]]];
        border: 1.5px solid [[[
          return states['person.chris']?.state === 'home' ? '#22c55e' : '#334155';
        ]]];
      ">C</div>

      <!-- Claire -->
      <div style="
        width: 36px; height: 36px;
        border-radius: 50%;
        display: flex; align-items: center; justify-content: center;
        font-size: 11px; font-weight: 700; letter-spacing: 0.02em;
        background: [[[
          return states['person.claire']?.state === 'home'
            ? 'rgba(34,197,94,0.2)' : 'rgba(100,116,139,0.15)';
        ]]];
        color: [[[
          return states['person.claire']?.state === 'home' ? '#22c55e' : '#64748b';
        ]]];
        border: 1.5px solid [[[
          return states['person.claire']?.state === 'home' ? '#22c55e' : '#334155';
        ]]];
      ">CL</div>

      <!-- Benjamin -->
      <div style="
        width: 36px; height: 36px;
        border-radius: 50%;
        display: flex; align-items: center; justify-content: center;
        font-size: 11px; font-weight: 700; letter-spacing: 0.02em;
        background: [[[
          return states['person.benjamin']?.state === 'home'
            ? 'rgba(34,197,94,0.2)' : 'rgba(100,116,139,0.15)';
        ]]];
        color: [[[
          return states['person.benjamin']?.state === 'home' ? '#22c55e' : '#64748b';
        ]]];
        border: 1.5px solid [[[
          return states['person.benjamin']?.state === 'home' ? '#22c55e' : '#334155';
        ]]];
      ">B</div>
    </div>
  </div>
```

---

## Entity ID Reference

| Element | Entity ID | Attribute |
|---|---|---|
| Time | sensor.time | state |
| Date | JavaScript Date object (no entity) | — |
| Alarm state | alarm_control_panel.kuprycz_home | state |
| Weather temp | weather.forecast_home | attributes.temperature |
| Weather cond | weather.forecast_home | state |
| Chris location | person.chris | state (home/away) |
| Claire location | person.claire | state (home/away) |
| Benjamin loc. | person.benjamin | state (home/away) |

---

## Notes on sensor.time

`sensor.time` must be enabled in HA. If it doesn't exist, enable it in `configuration.yaml`:

```yaml
sensor:
  - platform: time_date
    display_options:
      - 'time'
      - 'date'
```

Restart HA after adding. Entity will be `sensor.time` and `sensor.date`.

---

## Notes on Person Entity IDs

Check exact entity IDs in HA → Settings → People. Common formats: `person.chris`, `person.chris_kuprycz`

Update the YAML above to match your exact entity IDs.

---

## Common Failure Modes

| Symptom | Cause | Fix |
|---|---|---|
| Time shows `--:--` | sensor.time not enabled | Add time_date sensor to config.yaml |
| Alarm pill shows grey/Unknown | Wrong entity ID | Check alarm_control_panel entity ID in Developer Tools |
| Weather shows `--°` | weather entity unavailable | Verify weather.forecast_home exists |
| Avatars all grey | person entity IDs wrong | Check exact IDs in Settings → People |
| Label renders as raw HTML | button-card not loaded | Verify resource loaded in Settings → Dashboards → Resources |
| JS template errors in console | `[[[` syntax wrong | button-card uses `[[[` not `{{` for JS templates |
