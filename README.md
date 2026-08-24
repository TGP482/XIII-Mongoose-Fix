# XIII: Mongoose Fix
<img width="512" alt="XIII Mongoose Fix" src="https://github.com/user-attachments/assets/7bf18131-8965-4962-89f3-733b09dedf0d"/>

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
- **Resolution** - Sets the game's resolution, with 0 using your desktop resolution.
- **V-Sync** - Restores V-Sync support in windowed mode, which the game only ever applied in fullscreen.
- **Max Frame Rate** - 

### Graphics
- **Anisotropic Filtering** - Forces the selected anisotropic filtering level on all textures.

### Input
- **Mouse Sensitivity** - Adjusts mouse sensitivity, which the game has no option for.
- **Mouse Smoothing** - Removes the engine's mouse smoothing, which swallows fast movement.

### Field of View
- **Field of View** - Adjusts the field of view from the game's 85 default, range: 45 - 145. Weapon zoom and scopes keep their magnification.

### Mongoose Fixes
- Added raw mouse input, fixing mouse movement changing with your polling rate.
- Fixed an issue where mouse sensitivity changed with the field of view.

### Credits
- [ThirteenAG](https://github.com/ThirteenAG) - [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
