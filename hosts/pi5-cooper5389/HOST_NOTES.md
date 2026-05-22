# Pi 5 Host Configuration — cooper5389@192.168.51.179

## BlueZ --experimental flag (2026-05-21)

**What changed:** Created systemd drop-in override at
`/etc/systemd/system/bluetooth.service.d/override.conf` to add
`--experimental` to the bluetoothd ExecStart line.

**Why:** HA's Bluetooth integration requires passive BLE scanning.
On Linux, passive scanning requires BlueZ >= 5.56 running with
`--experimental` and kernel >= 5.10. The Pi had BlueZ 5.66 and
kernel 6.12 (both sufficient) but bluetoothd was launched without
`--experimental`, causing the integration to fail with "passive
scanning on Linux requires BlueZ >= 5.56 with --experimental
enabled."

**Diagnostic report:** See `BT_DIAGNOSTIC_REPORT.md` in the repo
root for full investigation details.

**Verification (2026-05-21):**
- bluetoothd ExecStart includes --experimental: YES
- bluetooth.service active (running): YES
- HA logs show zero bluetooth errors after restart: YES
- No "Failed to start Bluetooth" or "passive scanning" errors

**Rollback:**
```bash
sudo rm /etc/systemd/system/bluetooth.service.d/override.conf
sudo systemctl daemon-reload
sudo systemctl restart bluetooth
sudo docker restart homeassistant
```

**Files on Pi:**
- `/etc/systemd/system/bluetooth.service.d/override.conf` — the override
- `/lib/systemd/system/bluetooth.service` — original (untouched)
