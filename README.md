# XIII: Mongoose Fix
<img width="1280" alt="XIII Mongoose Fix" src="https://github.com/user-attachments/assets/7ac9050d-7654-48ea-9f21-1a66f37d4412" />

## Installation
The latest version of [XIII: Mongoose Fix](https://github.com/TGP482/XIII-Mongoose-Fix/releases) can be found in the Releases page.

### Game Setup
- After downloading XIII: Mongoose Fix, extract the contents to your XIII directory and overwrite all existing files when prompted.
- You can adjust the mod settings inside `XIIIMongooseFix.ini` located in the `system\plugins` folder, whilst the game is running.

> [!NOTE]
> Linux / Steam Deck will need the following command to launch the game:
```
WINEDLLOVERRIDES="dinput8=n,b" %command%
```

## Features

### Display
- **Display Mode** - Allows you to change between Fullscreen, Borderless and Windowed modes.
- **Resolution** - Sets the game's resolution, with 0 using your desktop resolution.
- **Internal Resolution** - Added the option to change the internal resolution to a custom value, to allow for supersampling and downscaling.
- **Scaling Filter** - Sets the internal resolution scaling filter.
- **V-Sync** - Restores V-Sync in windowed and borderless, which the game only ever applied in fullscreen.
- **Max Frame Rate** - Caps the game's frame rate, either at your monitor's refresh rate automatically or at a value you pick.

### Graphics
- **Anisotropic Filtering** - Forces the selected anisotropic filtering level on all textures instead of using the game's defaults.
- **MSAA** - Enabled multisample antialiasing at 2, 4 or 8 samples.

### Gameplay
- **Mouse Look Sensitivity** - Adjusts mouse look sensitivity, which the game has no option for.
- **Mouse Smoothing** - Removes the engine's mouse smoothing, which swallows fast movement.

### Field of View
- **Field of View** - Adjusts the base gameplay field of view.

### Controller
- **Restored Controller Support** - Full controller support has been restored.
- **Controller Look Sensitivity** - Adjusts controller look sensitivity.
- **Vibration** - Restores controller vibration support.
- **Layout** - Sets the starting pad layout out of the four the Xbox version shipped with.

### General
- **Skip Intro** - Skips the Ubisoft, alien and nvidia logo movies the exe plays before the menu.

### Mongoose Fixes
- Fixed mouse movement breaking with high polling rates and removed the deadzone.
- Fixed an issue where mouse sensitivity changed with the field of view.
- Fixed HUD elements, menus, blur effects and comic panels not scaling properly with resolution.
- Fixed the cutscene camera never turning onto its focus target above 162 fps, leaving the shot pointed at whatever angle it started on.
- Fixed getting stuck when squeezing through narrow gaps at high frame rates.

### Credits
- [ThirteenAG](https://github.com/ThirteenAG) - [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
