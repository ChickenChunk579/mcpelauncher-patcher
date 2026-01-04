# mcpelauncher-patcher
Patches the libraries for mcpelauncher (minecraft-linux) to solve the error:
`Error [MinecraftUtils] Failed to load Minecraft: dlopen failed: cannot locate symbol "__strlcpy_chk" referenced by "/home/rhys/mcpe/test/lib/x86_64/libPlayFabMultiplayer.so"...`
and later:
`Error [MinecraftUtils] Failed to load Minecraft: dlopen failed: cannot locate symbol "freopen64" referenced by "/home/rhys/mcpe/test/lib/x86_64/libPlayFabMultiplayer.so"...`

## NOTICE
This patcher is very untested. It seems to work, but the implementation of freopen64 may cause crashes as compiling with libc causes issues, so i had to leave out a shimed implementation of it. In future, a better library compiling with android NDK libc could fix those errors. This is mainly just a temporary solution to fix the patched libPlayFabMultiplayer.so not working found from [here](https://github.com/minecraft-linux/mcpelauncher-manifest/issues/1511)
