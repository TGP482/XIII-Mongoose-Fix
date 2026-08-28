copy bin\XIIIMongooseFix.asi data\system\plugins\XIIIMongooseFix.asi

7z a "XIIIMongooseFix.zip" ".\data\*" ^
-xr!*\.gitkeep
