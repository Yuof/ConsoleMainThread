# Console Main Thread

SKSE plugin for Skyrim Special Edition: SE 1.5.97 and AE 1.6.317-1.7.104, one DLL (tested on 1.6.1170). Runs the console's cell changes (`coc`, `coe`,
`cow`) and console batch files (`bat`) on the main thread, the way travel through a door does. Fixes, on 1.6.1170:

- `SkyrimSE.exe+14EAFFA` (`BSEffectShader::SetupGeometry`, a light that no longer exists) and `SkyrimSE.exe+14A6BBA`
  (`ShadowSceneNode::AccumulateLight`, a room's light list emptied while it is read) after a `coc` / `coe` / `cow` that
  comes from a script, a batch file or a tool rather than the open console;
- crashes in the interface after a batch file that changes cells (a freed file stream, menus advanced and drawn by
  two threads at once - with TrueHUD that is `TrueHUDMenu::AdvanceMovie`), and the freeze when such a batch file runs
  with the console open.

On this runtime a console command that does not come from the open console runs on a job worker thread, and the cell
change starts right there: the old cell is torn down, the scene's portal graph and light lists swapped, while the main
thread is still drawing the frame. A batch file runs its lines inside the interface's message processing. The plugin
queues both and runs them on the main thread at the start of its next loop; a `coc` typed into the open console is
already there and runs unchanged.

`ConsoleMainThread.log` lists every command and batch file it moved and when it ran. The plugin checks each code site
it patches and stays inactive, with a line in its log, on a build where they do not match.

## Requirements
- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)

## Building
Visual Studio 2022 or newer (C++; the preset uses the newest one installed), CMake 3.24+, [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

```
git clone --recurse-submodules <this repo>
cd ConsoleMainThread
cmake --preset release
cmake --build build --config Release
```

## License
GPL-3.0-or-later.
