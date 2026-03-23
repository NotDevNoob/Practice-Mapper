# Practice Mapper

Practice Mapper adds a dedicated `Practice` button to the online level menu in Geometry Dash and connects it to your approved level-mapping system.

## What It Does

- Adds a smaller Practice-style button near the level actions without hardcoding a fixed overlapping position
- Adjusts placement dynamically so it works more reliably across different screen sizes and aspect ratios
- Reads the current online level ID from the level menu
- Sends a request to your configured API to look up an approved practice mapping
- If a mapping exists, opens the mapped practice level immediately
- If no mapping exists, shows a small in-game message instead of interrupting the player

## Intended Workflow

This mod is meant to work with the rest of your mapping stack:

- players submit mappings through the Discord bot
- approved mappings are published through your backend and GitHub storage
- the mod reads those approved mappings at runtime from the API

In practice, that means an original level can point to a dedicated practice copy or StartPos version without the player needing to search for it manually.

## Settings

### API Base URL

Set this to the base address of the backend that serves approved mappings.

Example:

`http://127.0.0.1:3000`

## Notes

- This mod only adds the button on the online level menu
- It does not change the built-in pause menu practice button
- A working API is required for mapping lookup
- Only approved mappings are meant to open from the mod

## Troubleshooting

- If the button appears but nothing opens, check that the API is running and reachable
- If the mapped level fails to open, verify that the mapped level ID is still valid and publicly accessible
- If no mapping exists for a level, the mod will simply show a small notice
