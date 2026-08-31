## Features

### Display
- **Display Mode** - Allows you to change between Fullscreen, Borderless and Windowed modes.
- **Window Resolution** - Sets the game's window resolution, with 0 using your desktop resolution.
- **Internal Resolution** - Added the option to change the internal resolution to a custom value, to allow for supersampling and downscaling.
- **Scaling Filter** - Sets the internal resolution scaling filter.
- **V-Sync** - Restores V-Sync in windowed and borderless, which the game only ever applied in fullscreen.
- **Max Frame Rate** - Caps the game's frame rate, either at your monitor's refresh rate automatically or at a value you pick, bypassing the game's 60FPS lock.

### Graphics
- **DirectX Version** - Runs the game's renderer through DirectX 9 via d3d8to9 or the stock DirectX 8 path which the game ships with.
- **Anisotropic Filtering** - Forces the selected anisotropic filtering level on all textures instead of using the game's defaults.
- **MSAA** - Enables multisample antialiasing at 2, 4 or 8 samples.
- **CRT Gamma** - Restores the vibrance and contrast a CRT gave the game's colours. (0) off, (1) on. DirectX 9 only.

### Gameplay
- **Mouse Look Sensitivity** - Adjusts mouse look sensitivity.
- **Mouse Smoothing** - Toggles mouse smoothing on and off.

### Field of View
- **Field of View** - Adjusts the base gameplay field of view.

### Controller
- **Restored Controller Support** - Full controller support has been restored.
- **Controller Look Sensitivity** - Adjusts controller look sensitivity.
- **Vibration** - Restores controller vibration support.
- **Layout** - Sets the starting pad layout out of the four the Xbox version shipped with.

### General
- **Skip Intro** - Skips the Ubisoft, alien and nvidia logo movies the exe plays before the menu.
- **Allow Cheats** - Allows cheats to be used through the console.

### Fixes
- Fixed mouse movement breaking with high polling rates and removed the deadzone.
- Fixed an issue where mouse sensitivity changed with the field of view.
- Fixed HUD elements, menus, blur effects and comic panels not scaling properly with resolution.
- Fixed the cutscene camera never turning onto its focus target above 162 fps, leaving the shot pointed at whatever angle it started on.
- Fixed getting stuck when squeezing through narrow gaps at high frame rates.
- Fixed an issue with FMVs playing silently.

### Credits
- [ThirteenAG](https://github.com/ThirteenAG) - [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).
- [crosire](https://github.com/crosire) - [d3d8to9](https://github.com/crosire/d3d8to9).
- [CeeJayDK](https://github.com/CeeJayDK) - [SweetFX](https://github.com/CeeJayDK/SweetFX), CRT Gamma.
