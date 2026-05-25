# liboriinit

ADRV9001/ADRV9002 initialization library — encapsulates safe-state
enforcement, profile loading, and chip-state verification via libiio.

**Status: design phase.** Not yet implemented.

## Planned scope

A C library (with C ABI for embeddability) that:

1. Opens an libiio context, finds the ADRV9001/9002 PHY
2. Optionally loads a profile JSON via `profile_config` sysfs attribute
3. Sets per-channel LO frequencies, sample rates, gains
4. Enforces the "don't touch" list (never write `sync_start_enable`,
   never write `initial_calibrations=run` from `rf_enabled` state, etc.)
5. Verifies the chip ends up in the expected `rf_enabled` state
6. Returns clean error codes for all failure modes

## Why this exists

The ADRV9001/9002 has sharp edges that bite the unwary (see
`chip_init_constraints.md` in the Mode-Dynamic-Transponder repo, or
the future copy here). Encoding the safe-init logic in a library
means each new daemon or tool doesn't need to re-discover the rules
by breaking the board.

## See also

- `chip_init_constraints.md` (Haifuraiya repo) — the rules this library
  enforces
- `services/oriinit/` (future) — systemd oneshot service wrapping
  liboriinit for boot-time init
