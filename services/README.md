# services

systemd services for ORI ADRV9001/9002 SDR builds — telemetry
publishers, configuration daemons, and observability tooling.

**Status: design phase.** Not yet populated.

## Planned services

- **`oriinit`** — boot-time chip configuration. systemd oneshot,
  uses `liboriinit`. Exits when ADRV9001/9002 is in `rf_enabled`.
- **`hatsuon`** — per-channel demod telemetry publisher. Reads PL
  CSR registers, publishes JSON to MQTT for each of the 64 channels
  in the Haifuraiya demod farm.
- **`dvbs2-mon`** — DVB-S2 framer/modulator telemetry: FEC stats,
  MODCOD, frame rate, BBHEADER inspection.
- **`kabura-mon`** — Kabura-ya multiplexer telemetry: per-channel
  queue occupancy, scheduling stats.

Each follows the Takadono pattern: shell script (or Python with
paho-mqtt) reading registers via `devmem` or libiio attributes and
publishing JSON over MQTT at ~1 Hz.

## Why systemd

- Boot-time ordering and dependencies (oriinit must finish before
  observability publishers start)
- Automatic restart on failure
- Standard logging via journald
- Easy to enable/disable services per deployment
