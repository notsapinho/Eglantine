<!--
SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Pokémon Pokopia: keyboard and mouse profile

An input profile with Recompia's keyboard and mouse layout, for game version 2.0.1 with this build's Pokopia mouse features (mouse camera, Joy-Con mouse, menu clicks).

## Installing

1. Copy `Pokopia keyboard and mouse.ini` into Eglantine's `input` folder:
   - Windows: `%APPDATA%\eglantine\config\input\`
   - Linux and Steam Deck: `~/.config/eglantine/input/`
   (make the `input` folder if it is not there)
2. In **Emulation → Configure → Controls**, Player 1: pick **Pokopia keyboard and mouse** under Profile and press **Load**.
3. Controller type: **Dual Joycons** (the profile sets it; the Joy-Con mouse needs it).

## Layout

| Key | Button | In Pokopia |
| --- | --- | --- |
| W A S D | Left stick | Move |
| Arrow keys | Right stick | Camera |
| E | A | Interact |
| Space | R | Jump |
| Shift | B | Run, back |
| Tab | X | Bag |
| F | Y | Pick up |
| Q | L | Set move |
| Ctrl | Left stick click | |
| V | Right stick click | |
| M | + | Main menu (Esc is Eglantine's Exit Fullscreen) |
| C | − | Camera |
| 1 2 3 4 | D-pad up, down, left, right | Follow, emote, swap move |

The mouse:

| | Camera mode (mouse panning on) | Joy-Con mouse (middle click) | Menus |
| --- | --- | --- | --- |
| Move | Turns the camera | The game's pointer | Pointer |
| Left button | ZR (use a move) | R | Picks the entry under it |
| Right button | ZL | ZR | B |
| Wheel | D-pad up / down | D-pad up / down | D-pad up / down |
| Middle button | To the Joy-Con mouse | Back to the camera | |
