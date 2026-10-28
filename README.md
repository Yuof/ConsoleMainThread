# Disabled Attacker Crash Fix

SKSE plugin for Skyrim Special Edition (Anniversary Edition runtime) with
[Precision - Accurate Melee Collisions](https://www.nexusmods.com/skyrimspecialedition/mods/72347) 2.0.6. Fixes a crash
when an actor is disabled or unloaded in the middle of a melee swing (a script, a cell change): `SkyrimSE.exe+070DB20`
on 1.6.1170, under `Actor::CalculateCurrentHitTargetForWeaponSwing`, with Precision's `PendingHit::Run` on the stack.

Precision queues a melee hit at its contact and processes it on a later update. An attacker disabled in between has no
AI process any more; Precision passes it to the game's hit-target function, which reads through the missing process,
and its hitstop reads the process too. The plugin wraps `PendingHit::Run` and drops such a hit - there is no attacker
left to apply it. Every other hit runs unchanged.

The same check is proposed upstream: https://github.com/ersh1/Precision/pull/6. Until a Precision release has it, this
plugin covers the released 2.0.6 build only: it checks Precision.dll's build stamp and the function's first bytes and
stays inactive (with a line in `DisabledAttackerCrashFix.log`) for any other build.

## Requirements
- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- Precision - Accurate Melee Collisions 2.0.6

## Building
Visual Studio 2022 or newer (C++; the preset uses the newest one installed), CMake 3.24+, [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

```
git clone --recurse-submodules <this repo>
cd DisabledAttackerCrashFix
cmake --preset release
cmake --build --preset release
```

## License
GPL-3.0-or-later.
