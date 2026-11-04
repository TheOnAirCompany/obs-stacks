# Stacks for OBS — User guide (0.1.0 preview)

## Install

Copy `obs-stacks.plugin` to `~/Library/Application Support/obs-studio/plugins/`, restart OBS Studio (33.0.1+), then open **Docks > Stacks**. The preview build is unsigned; allow it in System Settings > Privacy & Security if macOS blocks it. To uninstall, delete the plugin folder.

## The dock

Scenes appear in a tree. The scene on air shows a **LIVE** badge, the scene in Preview (Studio Mode) shows **PVW**. New scenes created in OBS land in the **Unsorted** section.

## Going live

- Without Studio Mode: click a scene to switch to it.
- With Studio Mode: click sends the scene to Preview; double-click sends it to Preview and triggers the transition.
- **Safe click** (Stacks menu): without Studio Mode, a single click does nothing; double-click goes live.

## Organizing

- **Stacks**: New Stack, or New Stack from selection. Stacks are one level deep and collapse / expand; the Stacks menu has Expand all / Collapse all.
- **Move scenes**: drag & drop, or right-click > Move to Stack (or Top level).
- **Separators**: New Separator, with a label or empty for a plain line. Separators are never scenes and cannot be put on air.
- **Colors**: right-click > Color. 8 palette colors or a custom one. Scenes show a color strip; stacks are tinted.
- **Favorites**: right-click > Add to favorites; use the filter button to show favorites only.
- **Search**: type in the search field. It ignores case and accents.
- **Rename** stacks, separators and scenes from the context menu.

## Scenes

- **Add Scene** creates a scene in OBS (the name must be unique).
- **Delete Scene** asks for confirmation. Edit > Undo restores the scene, its sources and its place in its stack, without switching Program. A scene currently on air (Program) cannot be deleted.
- **Dissolve Stack** removes the stack only; its scenes move up to the top level and are never deleted.

## Settings (Stacks menu)

- Safe click.
- Hide native Scenes dock.

The interface is available in English and French, following OBS's language.

## Where the data is stored

Organization (stacks, separators, colors, favorites, order, collapsed state) is saved inside the active scene collection file, under the `obs-stacks` key, next to the scenes. Each scene is matched by its UUID, with a fallback on its name if the UUID is not found (for example after importing a collection). Scenes that are missing are shown as such and new ones go to Unsorted. The two settings above are stored in OBS's user configuration.

If a collection was organized by a newer version of the plugin, it is shown as a flat list and the data is kept unchanged.

## Removing the plugin

Stacks only adds its own data to the collection. Scenes, sources and settings are never modified, and OBS's native scene order is untouched. Uninstalling the plugin leaves everything working as before; the unused `obs-stacks` data is simply ignored.

## Not in 0.1.0

Icons, compact / organized modes, undo for organization changes, JSON export / import, nested stacks.
