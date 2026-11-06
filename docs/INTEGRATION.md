# Integration with other plugins

Stacks for OBS exposes a small interface on the **global proc handler**
(`obs_get_proc_handler()`) and the **global signal handler**
(`obs_get_signal_handler()`), so other plugins (Layouts for OBS first) can show the
organization of the scenes and file new scenes into stacks without linking against
Stacks. Everything is registered in `obs_module_load`; call the procs after
`OBS_FRONTEND_EVENT_FINISHED_LOADING`.

`proc_handler_call` returns false when Stacks is not installed: use it to detect Stacks.

The procs run on the UI thread. Called from another thread, they wait for the UI thread
(do not call them from a render or audio thread).

## `stacks_get_organization`

```
void stacks_get_organization(out string json)
```

`json` is the organization of the current scene collection, in the same schema as the
module data saved with the collection:

```json
{
  "schema_version": 1,
  "tree": [
    {"type": "stack", "name": "Plateau", "color": "#3E63DD", "icon": "mic",
     "collapsed": false, "children": [
       {"type": "scene", "uuid": "…", "name": "Duo", "favorite": true}
     ]},
    {"type": "separator", "label": "Ads"},
    {"type": "scene", "uuid": "…", "name": "BRB"}
  ]
}
```

* `color`, `icon`, `favorite`, `collapsed` are optional.
* Stacks nest (at most 4 levels).
* Scenes that Stacks has not filed yet ("Unsorted") are not in the tree; scenes that
  were deleted may stay as tombstones (`missing_since`): match scenes by `uuid` against
  the scenes that exist.
* Readers must ignore unknown node types and fields. `schema_version` increases only
  for incompatible changes.

An empty string means no organization is known yet (no collection loaded).

## `stacks_place_scene`

```
void stacks_place_scene(in string uuid, in string stack, out bool ok)
```

Moves the scene with this uuid into the stack named `stack` (at the end). For a nested
stack give the path with `" / "` between levels (`"Show / Plateau"`). A scene created a
moment ago is picked up first. It creates nothing: `ok` is false when the scene or the
stack does not exist, or while the organization is read-only. The move is one
Edit > Undo step, like a drag in the dock.

```c
calldata_t cd = {0};
calldata_set_string(&cd, "uuid", obs_source_get_uuid(scene));
calldata_set_string(&cd, "stack", "Plateau");
proc_handler_call(obs_get_proc_handler(), "stacks_place_scene", &cd);
bool ok = calldata_bool(&cd, "ok");
calldata_free(&cd);
```

## Signal `stacks_changed`

```
void stacks_changed()
```

Emitted on the global signal handler, on the UI thread, once after the organization
changed (stacks, order, colors, favorites, collapsed state, scenes added, renamed or
removed, collection switched). Several changes in a row give one signal. Read the new
organization with `stacks_get_organization`.

```c
signal_handler_connect(obs_get_signal_handler(), "stacks_changed", on_stacks_changed, data);
```
