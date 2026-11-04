# Stacks for OBS

**A better way to organize your OBS scenes.**

Stacks for OBS is a native OBS Studio plugin that adds a scene explorer dock: collapsible **stacks**, real **separators**, **colors**, **icons**, **favorites** and **search** — without ever creating fake scenes, touching your sources or rewriting your scene collections.

> **Status: Phase 1 — Discovery & feasibility.** No feature code yet. The product and technical documents below are drafts awaiting validation before implementation starts.

## Why

OBS Studio's Scenes panel is a flat list. As collections grow, people fake structure with `----------` scenes, `[LIVE]` prefixes and emoji. Those workarounds pollute the collection, show up in Stream Deck and obs-websocket, and are slow to scan during a live show.

Stacks keeps OBS as the single source of truth for scenes and only stores *organization metadata* next to them. Groups of scenes (stacks), separators, colors and icons exist only in the plugin's data:

- a separator can never be put on air, never appears in transitions, obs-websocket or Stream Deck;
- dissolving a stack never deletes a scene;
- disabling or uninstalling the plugin leaves every scene, source and setting exactly as it was.

## Planned features

| Area | Highlights |
|------|------------|
| Scene explorer dock | Replaces the native Scenes dock for daily use (opt-in), Program / Preview aware, Studio Mode safe |
| Stacks | Collapsible folders of scenes, color, icon, drag & drop, dissolve without deleting scenes |
| Separators | Line, labeled and spacer separators that are never scenes |
| Colors | 8-color palette, custom colors, strip / tinted / dot display modes |
| Icons | Built-in icon library, optional SVG / PNG import (sanitized) |
| Search & favorites | Instant, case- and accent-insensitive search; favorites filter |
| Display | Compact and Organized modes, density and visibility options |
| Languages | English and French |

## Documentation

| Document | Content |
|----------|---------|
| [Technical feasibility report](docs/01-technical-feasibility.md) | What OBS 33 allows, verified against its source code |
| [Product requirements](docs/02-product-requirements.md) | Goals, personas, user stories, MVP scope |
| [UX/UI specification](docs/03-ux-ui-specification.md) | Layouts, states, interactions, accessibility |
| [Software architecture](docs/04-software-architecture.md) | Components, data model, events, persistence, ADRs |
| [Implementation plan](docs/05-implementation-plan.md) | Phased, dependency-ordered tasks |
| [Testing strategy](docs/06-testing-strategy.md) | Unit, integration, manual, performance and data-safety tests |

## Compatibility

- **OBS Studio 33.0.1 or later** (uses the OBS 33 plugin layout and `manifest.json`).
- macOS 13+ (Apple Silicon and Intel, universal binary), Windows 10/11 x64, Linux (Ubuntu 26.04 `.deb`).

Stacks keeps its own order of scenes. OBS's native scene order — used by the Multiview, obs-websocket clients, Stream Deck and Companion — is never modified.

## Building

The build system comes from [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate), updated for OBS Studio 33.0.1 (obs-deps and Qt6 `2026-10-23`).

| Platform | Requirements |
|----------|--------------|
| macOS | Xcode 26.5 or newer (macOS SDK 26.5+), CMake 3.30.5+ |
| Windows | Visual Studio 18 2026, Windows SDK 10.0.26100.0, CMake 3.30.5+ |
| Ubuntu 26.04 | CMake 3.28.3+, `ninja-build`, `pkg-config`, `build-essential` |

```sh
# macOS
cmake --preset macos
cmake --build --preset macos

# Windows
cmake --preset windows-x64
cmake --build --preset windows-x64

# Ubuntu
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```

The first configure downloads the OBS 33.0.1 sources and prebuilt dependencies into `.deps/` and builds `libobs` and `obs-frontend-api` once.

### Plugin layout (OBS 33)

| Platform | Binary | Data (incl. `manifest.json`) |
|----------|--------|------------------------------|
| Windows  | `%ProgramData%/obs-studio/plugins/obs-stacks/obs-stacks.dll` | `%ProgramData%/obs-studio/plugins/obs-stacks/data` |
| macOS    | `~/Library/Application Support/obs-studio/plugins/obs-stacks.plugin/Contents/MacOS` | `obs-stacks.plugin/Contents/Resources` |
| Linux    | `<libdir>/obs-modules/plugins/obs-stacks.so` | `<datadir>/obs/obs-modules/plugins/obs-stacks` |

## License

[GPL-2.0-or-later](LICENSE), like OBS Studio.

© 2026 The OnAir Company
