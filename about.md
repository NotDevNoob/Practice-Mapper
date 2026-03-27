# Practice Mapper

Practice Mapper adds a dedicated `Practice` button to the online level menu in Geometry Dash and connects it to your approved level-mapping system.

## Join the Discord

Need help? Want to submit a mapping? Want to report one that opens the wrong level?

Join the Practice Mapper Discord:

`https://discord.gg/EG4s6VuuTE`

You can also open the mod page in Geode and tap the Discord icon to jump there directly.

## What It Does

- Adds a smaller Practice-style button near the level actions without hardcoding a fixed overlapping position
- Adjusts placement dynamically so it works more reliably across different screen sizes and aspect ratios
- Reads the current online level ID from the level menu
- Reads approved mappings from the public GitHub JSON file by default, with optional API fallback
- If a mapping exists, opens the mapped practice level immediately and downloads it first if needed
- If no mapping exists, shows a small in-game message instead of interrupting the player

## Intended Workflow

This mod is meant to work with the rest of your mapping stack:

- players submit mappings through the Discord bot
- approved mappings are published through your backend and GitHub storage
- the mod reads those approved mappings at runtime from the public mappings JSON or the fallback API

In practice, that means an original level can point to a dedicated practice copy or StartPos version without the player needing to search for it manually.

## Settings

### Approved Mappings URL

Set this to the public JSON file that contains approved mappings.

Example:

`https://raw.githubusercontent.com/NotDevNoob/gd-level-mappings/main/mappings.json`

### API Base URL

Optional fallback backend used only if the approved mappings JSON is unavailable.

## Notes

- This mod only adds the button on the online level menu
- It does not change the built-in pause menu practice button
- A public mappings JSON URL is enough for normal player installs
- Only approved mappings are meant to open from the mod

## Troubleshooting

- If the button does not appear, check that the mappings JSON URL is reachable and contains the level ID
- If the button appears but nothing opens, verify that the mapped level ID is still valid and publicly accessible
- If the mapped level fails to open, verify that the mapped level ID is still valid and publicly accessible
- If no mapping exists for a level, the mod will simply show a small notice
