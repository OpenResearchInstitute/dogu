# Contributing to dōgu

Thanks for your interest in contributing to dōgu.

## License

By contributing, you agree your work will be released under the
CERN-OHL-S v2 license used by this repository.

## What's welcome

- Bug fixes
- New tools that fit dōgu's scope (ARM-side ADRV9001/9002 tooling)
- Improvements to existing tools (better error handling, additional
  diagnostics, performance work)
- Yocto recipes for packaging
- Documentation improvements
- Testing on alternative platforms (other Zynq/ZynqMP boards with
  ADRV9001/9002)

## What's out of scope

- HDL or FPGA designs (those live in Mode-Dynamic-Transponder,
  pluto_msk, etc.)
- C++ demod implementations (see opv-cxx-demod)
- Tools for other transceivers (AD9361, AD9363, etc.) — those are
  better hosted elsewhere

## Pull request guidelines

- One concern per PR. Don't bundle unrelated changes.
- For new tools, include a Makefile and a README.md following the
  pattern of `tools/dma_listen/`.
- Test on real hardware if possible. If you don't have hardware
  access, note that in the PR description and another maintainer
  will validate.
- Follow existing code style (loose K&R-ish for C, 4-space indent).
- Keep commit messages descriptive. "Fix bug" is not descriptive.
  "Fix off-by-one in dma_listen byte counter" is.

## Issues and discussion

Open an issue for bugs, feature requests, or design discussion.
For larger architectural changes, open a discussion thread first
so we can talk through the approach before code is written.

## Code of conduct

Be respectful. ORI is a volunteer organization and all participation
is voluntary. Disagreements are welcome; bad behavior toward other
contributors is not.
