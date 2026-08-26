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
- **Display Mode** - Chooses between fullscreen, borderless and windowed.
- **Resolution** - Sets the game's resolution, with 0 using your desktop resolution.
- **V-Sync** - Restores V-Sync support in windowed mode, which the game only ever applied in fullscreen.
- **Max Frame Rate** - Caps the game's frame rate either at your monitor's refresh rate automatically or at a value you pick.

### Graphics
- **Anisotropic Filtering** - Forces the selected anisotropic filtering level on all textures.

### Input
- **Mouse Sensitivity** - Adjusts mouse sensitivity, which the game has no option for.
- **Mouse Smoothing** - Removes the engine's mouse smoothing, which swallows fast movement.

### Interface
- **Skip Intro Movies** - Skips the ubi, alien and nvidia logo movies the exe plays before the menu.

### Field of View
- **Field of View** - Adjusts the field of view from the game's 85 default, range: 45 - 145. Weapon zoom and scopes keep their magnification.

### Mongoose Fixes
- Fixed mouse movement breaking with high polling rates and removed the deadzone.
- Fixed an issue where mouse sensitivity changed with the field of view.
- Fixed HUD elements, menus, blur effects and comic panels not scaling properly with resolution.

### Credits
- [ThirteenAG](https://github.com/ThirteenAG) - [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
