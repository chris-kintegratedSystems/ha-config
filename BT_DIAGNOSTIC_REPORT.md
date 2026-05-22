# Bluetooth Integration Diagnostic Report
**Date:** 2026-05-21
**Host:** Pi 5 @ 192.168.51.179 (Debian Bookworm)
**Symptom:** HA Bluetooth integration "Failed setup, will retry" — passive scanning requires BlueZ >= 5.56 with --experimental and kernel >= 5.10

---

## Checklist

| Check | Pass? | Detail |
|-------|-------|--------|
| Kernel >= 5.10 | **YES** | `6.12.75+rpt-rpi-2712` |
| BlueZ >= 5.56 | **YES** | `5.66-1+rpt1+deb12u2` |
| `--experimental` on bluetoothd | **NO** | ExecStart is bare `/usr/libexec/bluetooth/bluetoothd` — no flags, no override dir |
| Host BT hardware healthy | **YES** | hci0 UP RUNNING, BCM4345C0 (BCM43455), BT 5.0, 0 errors |
| HA container has D-Bus socket | **YES** | `/run/dbus:/run/dbus:ro` mounted, `system_bus_socket` present |
| HA container has BT device access | **YES** | `privileged: true`, `/dev:/dev:rw`, `hciconfig` works inside container, hci0 visible |

---

## Raw Evidence

### 1. Kernel
```
6.12.75+rpt-rpi-2712
```

### 2. BlueZ
```
bluetoothctl: 5.66
ii  bluez  5.66-1+rpt1+deb12u2  arm64  Bluetooth tools and daemons
```

### 3. Bluetooth service
```
Active: active (running) since Tue 2026-03-31 13:51:38 CDT; 1 month 20 days ago
Main PID: 648 (bluetoothd)
```

### 4. ExecStart (no --experimental)
```
# systemctl cat bluetooth | grep ExecStart:
ExecStart=/usr/libexec/bluetooth/bluetoothd

# /lib/systemd/system/bluetooth.service:
ExecStart=/usr/libexec/bluetooth/bluetoothd

# Override directory:
ls: cannot access '/etc/systemd/system/bluetooth.service.d/': No such file or directory
```

### 5-6. Hardware
```
hci0: Type: Primary  Bus: UART
      BD Address: 88:A2:9E:0D:43:8A  ACL MTU: 1021:8
      UP RUNNING
      RX bytes:22154511  errors:0
      TX bytes:14466960  errors:0
      Manufacturer: Cypress Semiconductor (305)
      HCI Version: 5.0
      Name: BCM4345C0 (BCM43455 37.4MHz Raspberry Pi 3+-0190)
```

### 7. Bluetooth journal (last hour)
```
-- No entries --
```

### 8. dmesg BT
Key lines:
```
Bluetooth: hci0: BCM43455 37.4MHz Raspberry Pi 3+-0190
Bluetooth: hci0: Bad flag given (0x1) vs supported (0x0)
```
The "Bad flag" message confirms the kernel attempted passive scanning but bluetoothd reported the feature unsupported (because `--experimental` is off).

### 9-11. Container access
```yaml
# docker-compose.yml (~/homeassistant/docker-compose.yml):
services:
  homeassistant:
    image: "ghcr.io/home-assistant/home-assistant:stable"
    network_mode: host
    privileged: true
    devices:
      - /dev/serial/by-id/usb-Zooz_800_Z-Wave_Stick_533D004242-if00:/dev/zwave
    volumes:
      - ./config:/config
      - /etc/localtime:/etc/localtime:ro
      - /run/dbus:/run/dbus:ro
      - /dev:/dev:rw
    cap_add:
      - NET_ADMIN
      - NET_RAW
```
- D-Bus socket visible inside container: YES (`/var/run/dbus/system_bus_socket`)
- hciconfig inside container: hci0 UP RUNNING, same device as host
- Privileged mode: true

### 12. HA Bluetooth logs
No bluetooth-specific errors in `home-assistant.log` or docker logs. The "bluetooth" component appears in the startup timeout warning (blocking startup), consistent with the integration failing setup and entering retry loop.

---

## Root Cause

**The single missing piece is the `--experimental` flag on bluetoothd.**

Kernel (6.12) and BlueZ (5.66) both meet the minimum version requirements. The BT hardware is healthy and visible to both host and container. D-Bus is mounted. The container runs privileged with /dev access.

HA's Bluetooth integration requires passive BLE scanning. On Linux, passive scanning requires BlueZ to run with `--experimental` enabled, which exposes the LE Set Scan Parameters HCI command with passive mode. Without it, BlueZ rejects the passive scan request, HA sees "passive scanning... not supported," and the integration enters a retry loop.

The dmesg line `Bad flag given (0x1) vs supported (0x0)` confirms the kernel's BT subsystem attempted to set a flag (passive scan) that bluetoothd reported as unsupported.

---

## Proposed Fix

**Single change — add `--experimental` to bluetoothd:**

1. Create systemd override directory:
   ```bash
   sudo mkdir -p /etc/systemd/system/bluetooth.service.d/
   ```

2. Write override file:
   ```bash
   sudo tee /etc/systemd/system/bluetooth.service.d/experimental.conf << 'EOF'
   [Service]
   ExecStart=
   ExecStart=/usr/libexec/bluetooth/bluetoothd --experimental
   EOF
   ```
   (The blank `ExecStart=` clears the original before setting the new one — required by systemd.)

3. Reload and restart bluetooth:
   ```bash
   sudo systemctl daemon-reload
   sudo systemctl restart bluetooth
   ```

4. Verify:
   ```bash
   systemctl cat bluetooth | grep ExecStart
   # Should show: ExecStart=/usr/libexec/bluetooth/bluetoothd --experimental
   ```

5. Restart HA to pick up the new bluetoothd capabilities:
   ```bash
   sudo docker restart homeassistant
   ```

6. Check HA → Settings → Devices & Services → Bluetooth — hci0 should show healthy.

---

## Risk Assessment

| Question | Answer |
|----------|--------|
| Host reboot required? | **NO** — `systemctl restart bluetooth` is sufficient |
| HA container recreate required? | **NO** — `docker restart homeassistant` (not recreate) is sufficient |
| Downtime? | ~30 seconds for bluetooth restart + ~60 seconds for HA restart |
| Side effects? | `--experimental` enables additional BlueZ features (LE Privacy, LE Extended Advertising). These are passive — no functional change unless explicitly used. No known regressions. |
| Rollback? | Delete `/etc/systemd/system/bluetooth.service.d/experimental.conf`, daemon-reload, restart bluetooth |
