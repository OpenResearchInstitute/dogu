# TES profile files for Haifuraiya bring-up

These are TES-generated ADRV9002 profile files (`.json`) and matching SSI
stream microcode (`.bin`), packaged so they ship to the target with
`make deploy` and are immediately usable by `bring-up.sh`.

Naming convention follows the TES export pattern:
  tes_<tes-version>_<radio>_<mode>_<rate>_<bandwidth>.{bin,json}

Both files share the basename — `bring-up.sh <basename>` finds both.
