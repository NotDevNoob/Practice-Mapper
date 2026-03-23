# Practice Mapper

Practice Mapper is a Geode mod for Geometry Dash that adds a small Practice button to the online level menu.

When the button is pressed, the mod checks a configured API for an approved practice mapping for the current level.
If a mapped practice level exists, it opens that level immediately.
If no mapping exists, or if the mapped level cannot be found, the mod shows a small message instead of sending the player to the wrong level.

## Features

- Adds a Practice button to the online level menu
- Places the button dynamically to avoid overlapping existing UI
- Adapts to different screen sizes and aspect ratios
- Looks up approved mappings from a backend API
- Opens mapped practice levels directly
- Shows a popup when a mapped level ID is invalid

## Setting

- `API Base URL`: the backend address used for mapping lookups

Example:

`http://127.0.0.1:3000`

## Files

- `src/main.cpp`: main mod logic
- `mod.json`: Geode metadata
- `about.md`: detailed mod description shown in Geode
- `logo.png`: mod icon shown in Geode

## Build

This project targets Windows and is built with Geode.

Typical build command:

```powershell
geode build
```
