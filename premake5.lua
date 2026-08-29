newoption {
    trigger     = "with-version",
    value       = "STRING",
    description = "Current version",
}

workspace "XIIIMongooseFix"
   configurations { "Release", "Debug" }
   architecture "x86"
   location "build"
   cppdialect "C++latest"
   kind "SharedLib"
   language "C++"
   targetdir "data/system/plugins"
   implibdir "build/%{cfg.buildcfg}"
   symbolspath "build/%{cfg.buildcfg}/%{prj.name}.pdb"
   targetextension ".asi"
   buildoptions { "/dxifcInlineFunctions-" }

   defines { "rsc_CompanyName=\"XIIIMongooseFix\"" }
   defines { "rsc_LegalCopyright=\"MIT license\""}
   defines { "rsc_InternalName=\"%{prj.name}\"", "rsc_ProductName=\"%{prj.name}\"", "rsc_OriginalFilename=\"%{cfg.buildtarget.name}\"" }
   defines { "rsc_FileDescription=\"XIII Mongoose Fix\"" }
   defines { "rsc_UpdateUrl=\"https://github.com/TGP482/XIII-Mongoose-Fix\"" }

   local major = os.date("%d")
   local minor = os.date("%m")
   local build = os.date("%Y")
   local revision = os.date("%H") .. os.date("%M")

   if _OPTIONS["with-version"] then
      local t = {}
      for i in _OPTIONS["with-version"]:gmatch("([^.]+)") do
         t[#t + 1], _ = i:gsub("%D+", "")
      end
      while #t < 4 do t[#t + 1] = 0 end
      major    = math.min(tonumber(t[1]), 255)
      minor    = math.min(tonumber(t[2]), 255)
      build    = math.min(tonumber(t[3]), 65535)
      revision = math.min(tonumber(t[4]), 65535)
   end

   -- -- Local builds show DEV, CI releases use the actual version
   local nRelease = 1
   local szVersion = _OPTIONS["with-version"] and ("V" .. major) or ("V" .. nRelease .. " DEV")

   local githash = ""
   local f = io.popen("git rev-parse --short HEAD")
   if f then
      githash = f:read("*a"):gsub("%s+", "")
      f:close()
   end

   local productVersion = major .. "." .. minor .. "." .. build .. "." .. revision
   if githash ~= "" then
      productVersion = productVersion .. "-" .. githash
   end

   defines { "rsc_FileVersion_MAJOR=" .. major }
   defines { "rsc_FileVersion_MINOR=" .. minor }
   defines { "rsc_FileVersion_BUILD=" .. build }
   defines { "rsc_FileVersion_REVISION=" .. revision }
   defines { "rsc_FileVersion=\"" .. major .. "." .. minor .. "." .. build .. "\"" }
   defines { "rsc_ProductVersion=\"" .. productVersion .. "\"" }
   defines { "rsc_GitSHA1=\"" .. githash .. "\"" }
   defines { "rsc_GitSHA1W=L\"" .. githash .. "\"" }
   defines { "rsc_Version=\"" .. szVersion .. "\"" }
   defines { "rsc_VersionW=L\"" .. szVersion .. "\"" }

   defines { "_CRT_SECURE_NO_WARNINGS" }
   -- d3d8to9 keeps its own log file; the fix has one.
   defines { "D3D8TO9NOLOG" }

   includedirs { "source" }
   includedirs { "source/includes" }
   files { "source/**.h", "source/**.hpp", "source/**.cpp", "source/**.hxx", "source/**.ixx" }
   files { "source/resources/Versioninfo.rc" }

   files { "source/d3d8to9/MemoryModule/MemoryModule.c" }
   -- The two redistributables are megabytes of hex; listing them only slows the IDE down.
   removefiles { "source/d3d8to9/*_data_*.h" }
   links { "d3d9" }

   includedirs { "external/hooking" }
   includedirs { "external/injector/include" }
   includedirs { "external/injector/safetyhook/include" }
   includedirs { "external/injector/zydis" }
   includedirs { "external/inireader" }
   files { "external/hooking/Hooking.Patterns.h", "external/hooking/Hooking.Patterns.cpp" }
   -- Each safetyhook/zydis source is listed explicitly. Globbing the submodules yields a project
   -- that links only until something calls safetyhook.
   files { "external/injector/safetyhook/include/safetyhook.hpp" }
   files {
      "external/injector/safetyhook/src/allocator.cpp",
      "external/injector/safetyhook/src/easy.cpp",
      "external/injector/safetyhook/src/inline_hook.cpp",
      "external/injector/safetyhook/src/mid_hook.cpp",
      "external/injector/safetyhook/src/os.windows.cpp",
      "external/injector/safetyhook/src/utility.cpp",
      "external/injector/safetyhook/src/vmt_hook.cpp",
   }
   files { "external/injector/zydis/Zydis.h", "external/injector/zydis/Zydis.c" }
   files { "data/system/plugins/*.ini" }

   characterset ("Unicode")

   pbcommands = {
      "for %%P in (\"!MFDIR!.\") do set \"MFDIR=%%~fP\"",
      -- Parentheses keep both SET commands inside the FOR loop body.
      "for %%S in (\"$(TargetPath)\") do (set \"MFSRC=%%~fS\" & set \"MFNAME=%%~nxS\")",
      "set \"MFDST=!MFDIR!\\!MFNAME!\"",
      -- No game install found, skip
      "if not exist \"!MFDIR!\\\" goto :MFDONE",
      "if /I \"!MFSRC!\"==\"!MFDST!\" goto :MFDONE",
      -- Fail loudly: the game holds the asi open, so a silent copy failure leaves the old build in place.
      "copy /y \"!MFSRC!\" \"!MFDST!\" >nul || (echo XIIIMongooseFix: could not replace \"!MFDST!\", close the game and build again & endlocal & exit /b 1)",
      ":MFDONE",
      "endlocal",
      "exit /b 0" }

   function setpaths (gamepath, exepath, pluginspath)
      pluginspath = pluginspath or "plugins/"
      if (gamepath) then
         -- Keep deployment variables local to this build step
         local cmdcopy = {
            "setlocal EnableExtensions EnableDelayedExpansion",
            "set \"MFDIR=" .. (gamepath .. pluginspath):gsub("([^/\\])$", "%1/") .. "\"",
         }
         for _, cmd in ipairs(pbcommands) do
            table.insert(cmdcopy, cmd)
         end
         postbuildcommands (cmdcopy)
         debugdir (gamepath)
         if (exepath) then
            debugcommand (gamepath .. exepath)
            dir, file = exepath:match'(.*/)(.*)'
            debugdir (gamepath .. (dir or ""))
         end
      end
      targetdir ("data/system/plugins")
   end

   filter "configurations:Debug"
      defines { "DEBUG" }
      symbols "On"

   filter "configurations:Release"
      defines { "NDEBUG" }
      optimize "On"
      staticruntime "On"

project "XIIIMongooseFix"
   setpaths("C:/Program Files (x86)/Steam/steamapps/common/XIII - Classic/", "system/XIII.exe", "system/plugins/")
