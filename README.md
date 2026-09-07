# ps3-usb-autopair

Small Linux helper that pairs genuine Sony PlayStation 3 controllers to the
host Bluetooth adapter when they are connected over USB. The binary uses only
libc and the kernel `hidraw` API; it does not link to libusb or libbluetooth.

## Supported controllers

Both the original **Sixaxis** (CECHZC1) and **DualShock 3** (CECHZC2) use USB
ID `054c:0268` and the same cable-pairing feature reports, so both are covered
by the udev rule.

Third-party controllers that faithfully spoof `054c:0268` may work, but clone
protocols vary and are not guaranteed. Sony product `054c:0306` is the PS3 BD
remote, not a game controller, and is deliberately excluded.

## Build and stage

```bash
make
make install DESTDIR=/path/to/package-root
```

The staged files are:

- `/usr/sbin/ps3-usb-autopair`
- `/usr/lib/udev/rules.d/99-ps3-usb-autopair.rules`
- `/usr/lib/systemd/system/ps3-usb-autopair@.service`

Packaging and distribution metadata intentionally live outside this repository.

Use either this helper or BlueZ's built-in `sixaxis` plugin, never both. Both
watch the same hidraw event and can race while changing the controller's master
address and BlueZ device record.

## Runtime requirements

- BlueZ (`bluetooth.service`)
- systemd and udev
- a kernel with `hidraw` and `hid-sony` support
- root privileges for the systemd pairing unit
- persistent `/var/lib/bluetooth` storage across boots

BlueZ 5.83 or newer includes explicit cable-pairing support. The helper writes
`CablePairing=true`, allowing a PS3 controller to reconnect without weakening
the policy for every Bluetooth input device.

On older BlueZ versions that do not implement cable pairing, the image builder
must set this in `/etc/bluetooth/input.conf` and restart Bluetooth:

```ini
[General]
ClassicBondedOnly=false
```

That compatibility setting permits unbonded classic HID devices globally and
has security implications. Prefer a BlueZ version with cable-pairing support.

## RePlayOS BlueZ configuration

The current RePlayOS build uses Debian Trixie, whose BlueZ package is version
5.82 and has the `sixaxis` plugin enabled. When packaging this helper, disable
that plugin with a systemd drop-in owned by the RePlayOS package:

```ini
# /usr/lib/systemd/system/bluetooth.service.d/20-ps3-usb-autopair.conf
[Service]
ExecStart=
ExecStart=/usr/libexec/bluetooth/bluetoothd --noplugin=sixaxis
```

BlueZ 5.82 also needs `ClassicBondedOnly=false` as described above. The safer
long-term RePlayOS configuration is to upgrade or backport BlueZ's cable-pairing
support, while still disabling its `sixaxis` plugin when this helper is used.

After installing or upgrading the RePlayOS package, reload the unit definitions
and restart Bluetooth:

```bash
systemctl daemon-reload
systemctl restart bluetooth.service
```

Confirm the effective command before testing a controller:

```bash
systemctl show bluetooth.service --property=ExecStart
```

It should include `--noplugin=sixaxis`.

Reload udev after installing the helper files:

```bash
udevadm control --reload-rules
```

The template unit is started by udev and must not be enabled directly.

## Use

1. Connect the controller with a USB data cable.
2. Wait until `/run/ps3-usb-autopair/status` reports `waiting_ps`.
3. Disconnect USB and press the PS button.

The controller stores the host adapter address, while the helper stores the
trusted HID/SDP record under `/var/lib/bluetooth`. Both survive a normal reboot;
USB reconnection should not be necessary on later boots.

Inspect status with:

```bash
/usr/sbin/ps3-usb-autopair --status
```

Possible states are `pairing`, `waiting_ps`, and `error`.

## Multiple Bluetooth adapters

The helper selects `hci0` when present, then another live `hci*` adapter. It
does not use stale directory names from `/var/lib/bluetooth` as adapter IDs.
