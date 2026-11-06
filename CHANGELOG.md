# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.1] - 2026-11-06

### Changed

- The macOS package is signed (Developer ID) and notarized: macOS installs it without warning.

## [0.1.0] - 2026-11-04

First working preview (unsigned).

### Changed
- README: badges, English screenshots and per-platform install instructions.
- "+" button next to the options button for adding scenes, stacks, separators and spacers.
- The favorite star keeps a fixed slot at the far right; the PGM / PVW badge sits on its left.
- Destructive confirmations use localized Delete / Cancel buttons, with Cancel as default and Escape button.
- Design review applied: dock title "Scenes", PGM badge instead of LIVE, search and options below the list.

### Added
- PGM / PVW badge on collapsed stacks and optional reveal of the Program scene.
- "Send to Program" in Studio Mode.
- Missing scenes: optional display, clean up, soft and hard caps.
- Rolling backups and restore.
- Native scene actions: duplicate, filters, projector, screenshot, multiview, transition override.
- Inline rename, search and move shortcuts.
- Spacer separators, color display modes, icon / badge / count visibility options.
- Pinned "Favorites" section mirroring favorite scenes.
- First-run placement of the dock and Hide / Keep both / Not now dialog.
- No close button on the Stacks dock, kept when OBS locks or unlocks docks.
- Stacks dock with Program (LIVE) and Preview (PVW) badges.
- Studio Mode aware click and double-click, and an optional safe click setting.
- Stacks with collapse and expand, nestable up to 4 levels.
- Lucide icons (ISC) with a searchable picker.
- Undo / redo of organization changes through OBS Edit > Undo.
- Undoable scene deletion (same data as OBS, restores the stack placement, never switches Program).
- JSON export and import of the organization.
- Organized and Compact display modes.
- Separators, as a line or with a label.
- 8-color palette and custom color, shown as a strip and as a stack tint.
- Favorites and a favorites-only filter.
- Accent- and case-insensitive search.
- Unsorted section for new scenes.
- Drag & drop and Move to Stack.
- Add, rename and delete scene, with delete confirmation and refusal to delete the scene on air.
- Dissolve a stack without deleting its scenes.
- Optional hiding of the native Scenes dock.
- Keyboard: F2 / Return to rename, Delete / Backspace to delete.
- English and French UI.
- Organization persisted inside the scene collection, with UUID matching and name fallback.
- Project scaffold based on obs-plugintemplate, updated for OBS Studio 33.0.1 (obs-deps and Qt6 2026-10-23, OBS 33 plugin layout with `manifest.json`).
- Phase 1 documents: technical feasibility report, product requirements, UX/UI specification, software architecture, implementation plan and testing strategy.
