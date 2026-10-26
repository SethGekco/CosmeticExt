# CosmeticExt — notes for agents

- Presentation-only Syringe DLL for YR. **Never** touch simulation state or the
  synced randomizer. Audio uses `Randomizer::Global` (0x886B88, unsynced); keep
  it that way. If a feature needs synced state, it belongs in another DLL.
- Every commit adds a CHANGELOG.md entry (newest first, say why). No personal
  email anywhere in the repo.
- Gate commits on `tests/run_tests.sh` (C++ model checks + baker tests; exit
  status is the verdict). The DLL itself only builds in CI (MSVC v142,
  YRpp inline asm); local MinGW can't build it.
- Model maths lives in `src/Sound/DistanceModel.*` (no YRpp, host-testable).
  Keep engine access in `DistanceSound.cpp`. Defaults are duplicated in
  `DistanceModel.h` (`Resolved`) and `docs/soundmd-tags.md`: change both
  together.
- `VocClass::Array` items are handles; dereference once (`VocData(i)`). YRpp's
  `VocClass::Find` is wrong for this reason.
- Hooks: run `tools/check_hook_bounds.py` and `check_hook_overlap.py` against
  the YR Hook Encyclopedia registry (CI does). 0x750C9B steals a relative
  `je`, so it must never `return 0`.
- `tools/mixread.py` is vendored from MapForge; fix bugs upstream first and
  re-vendor, keeping the header line.
- Deploy: live game dir is `~/snap/cncra2yr/common/.wine/drive_c/Westwood/RA2/`.
  Back up first, then register `-i=CosmeticExt.dll` in both `wine-game.sh` and
  `ClientDefinitions.ini`, and confirm via `syringe.log` + `debug/debug.log`
  (`[CosmeticExt] loaded (built …)`).
