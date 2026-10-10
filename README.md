# mcpelauncher-patcher
Patches the libraries for mcpelauncher (minecraft-linux) to solve the error:
`Error [MinecraftUtils] Failed to load Minecraft: dlopen failed: cannot locate symbol "__strlcpy_chk" referenced by "/home/rhys/mcpe/test/lib/x86_64/libPlayFabMultiplayer.so"...`
and later:
`Error [MinecraftUtils] Failed to load Minecraft: dlopen failed: cannot locate symbol "freopen64" referenced by "/home/rhys/mcpe/test/lib/x86_64/libPlayFabMultiplayer.so"...`

## NOTICE
This patcher is very untested. It seems to work, but the implementation of freopen64 may cause crashes as compiling with libc causes issues, so i had to leave out a shimed implementation of it. In future, a better library compiling with android NDK libc could fix those errors. This is mainly just a temporary solution to fix the patched libPlayFabMultiplayer.so not working found from [here](https://github.com/minecraft-linux/mcpelauncher-manifest/issues/1511)

# Dependencies
* patchelf
* clang

# MCPE Launcher
[`launcher/`](launcher/) contains a GTK4 + libadwaita desktop launcher for
mcpelauncher-client. It manages multiple Minecraft: Bedrock Edition versions
side by side:

- upload any APK, validates it is a Minecraft APK (`com.mojang.minecraftpe`)
  *and* contains `lib/x86_64/` native binaries before allowing install,
- extracts each version into its own game directory,
- keeps a separate per-version data directory so versions don't share save
  data,
- launches with `mcpelauncher-client -dg <game dir> -dd <data dir>`.

It has no `unzip`/Java dependencies: it ships a small zip reader (`zip.c`) and
a binary-`AndroidManifest.xml` parser (`axml.c`).

## Requirements
* meson + ninja
* GTK 4, libadwaita (1.5 or newer), zlib development headers
* `mcpelauncher-client` built and installed (e.g. via `patch.sh` + `meson
  install` into `~/.local/bin`)

## Build & install
```sh
cd launcher
./install.sh        # builds and installs into ~/.local
~/.local/bin/mcpe-launcher
```

The client binary is looked up in this order: the path configured in
Settings → `~/.local/bin/mcpelauncher-client` → the `$PATH`.

## Layout
By default, versions and saves live under
`~/.local/share/mcpe-launcher/`:

```
~/.local/share/mcpe-launcher/
├── versions/<label>/    # extracted game files
├── data/<label>/        # per-version saves (passed via -dd)
├── logs/<label>.log     # client output per launch
└── installed.conf       # installed versions metadata
```

Settings are stored in `~/.config/mcpe-launcher/settings.conf`.