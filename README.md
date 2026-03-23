# Practice Mapper

Practice Mapper adds a small Practice button to the online level menu in Geometry Dash. When a mapped practice level exists, the mod opens it immediately so players can jump straight into the practice copy without searching for it manually.

## Compatibility

- Geometry Dash `2.208.1`
- Geode `5.4.1`
- Windows

## What the Mod Does

- adds a practice-style button near the main level actions
- places the button dynamically to avoid overlapping existing UI
- looks up approved mappings through the configured API
- opens the mapped practice level if it exists
- shows a small notice if no mapping is available
- blocks invalid fallback opens such as redirecting to level `1`

## Discord

Need help, want to submit mappings, or want to report a broken one?

- Discord: `https://discord.gg/CXHNThXEN6`
- GitHub: `https://github.com/NotDevNoob/Practice-Mapper`

## Settings

The mod exposes one setting in Geode:

- `API Base URL` - the backend endpoint used for mapping lookups, for example `http://127.0.0.1:3000`

## Build

This repo is configured for Geode `5.4.1`. If you are building locally with the project-local SDK clone:

```powershell
$env:GEODE_SDK='C:\Users\andre\Desktop\startpos\_geode-sdk-5.4.1'
cd C:\Users\andre\Desktop\startpos\geode-mod
geode build
```

## Automation

GitHub Actions is set up to:

- build the mod on pushes and pull requests
- upload the built `.geode` as a workflow artifact
- create a GitHub release with the built `.geode` when a tag like `v1.0.0` is pushed
