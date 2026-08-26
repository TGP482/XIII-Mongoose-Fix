## XIII Mongoose Fix

A fix for the 2003 PC release of XIII, built as an ASI plugin. Nothing in the game folder is
modified; removing the plugin puts the game back as it was.

### Installation

1. Extract the archive into the game folder, so `dinput8.dll` sits next to `XIII.exe` and
   `XIIIMongooseFix.asi` sits in `system\plugins`.
2. Edit `system\plugins\XIIIMongooseFix.ini` to taste. Every setting is live - save the file with
   the game running and it applies straight away.

### In this release

- **Raw mouse input**, one summed delta per frame, so the feel no longer changes with the mouse's
  polling rate.
- **Mouse smoothing can be turned off**, which removes the ±1 clamp that swallowed fast flicks and
  the tail that kept the view moving after the mouse stopped.
- **Mouse sensitivity multiplier**, plus the option to stop sensitivity changing with the field of
  view.
- **Anisotropic filtering** up to 16x. The renderer never set a maximum anisotropy at all, so this
  was not available at any quality setting.
- **V-Sync windowed as well as fullscreen.** The game's own option only ever applied fullscreen.
- **Field of view** between 45 and 145 degrees. Weapon zoom and scopes keep their magnification.
- **Frame rate cap** raised or removed. The game ships capped at 120.
- **Resolution** set from the ini, with 0 meaning your desktop resolution.
- **Comic panel scaling**, fixing interface metrics that were pinned to a 480p screen.

### Notes

- Above roughly 2000 FPS the world clock runs fast, because the engine clamps the frame delta to a
  0.0005s floor. Set a cap if that matters.
- Multiplayer tick rates are left alone.
