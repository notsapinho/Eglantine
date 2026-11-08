<!--
SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Texture mods

Eglantine can save the textures a game uploads and replace them with edited ones
(`src/video_core/texture_cache/texture_mods.h`). Textures are matched by a hash of the game's own
data, so a replacement only applies to that exact texture.

## 1. Dump

Start Eglantine with `EDEN_TEXTURE_DUMP` set to an empty folder, play until the textures you want
have been on screen, and quit.

- Windows (Command Prompt, in Eglantine's folder): `set EDEN_TEXTURE_DUMP=C:\eglantine-dump` then `eglantine.exe`
- Linux / Steam Deck: `EDEN_TEXTURE_DUMP=$HOME/eglantine-dump ./eglantine.AppImage` (Steam launch options: `EDEN_TEXTURE_DUMP=/home/deck/eglantine-dump %command%`)

Each texture is written once as `<hash>_<w>x<h>.bin` and `.json`. Leave the variable unset in normal
play: dumping slows texture loading.

## 2. Find and edit

```sh
pip install texture2ddecoder etcpak pillow numpy
python -I texture_mod_tool.py png  <dump folder> <png folder>
```

Edit the PNG of the texture you want (same size, keep the alpha), then:

```sh
python -I texture_mod_tool.py pack <dump folder>/<hash>_<w>x<h>.json edited.png "<load folder>/<title id>/<mod name>"
```

The load folder is the one **Open Mod Data Location** opens, one level up: `%APPDATA%\eglantine\load`
on Windows, `~/.local/share/eglantine/load` on Linux. Restart the game; the log says
`Texture replacement <name>: in use` when it applies.

`python -I texture_mod_tool.py icons <prompt_icons.pak> <folder>` extracts the keyboard and mouse
icons of Recompia's prompt pack (Kenney Input Prompts, CC0) for button-glyph mods.
