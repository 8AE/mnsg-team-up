# Team Up implementation

## Behavior

Team Up maintains three stable companion slots. On each gameplay frame it reads the four recruited-character values from `D_8015C608_15D208 + 0x94..0xA0`, excludes the active character in `0x8015C5DC`, and fills the remaining slots with unlocked characters in the game's stable order:

| ID | Character |
| ---: | --- |
| 0 | Goemon |
| 1 | Ebisumaru |
| 2 | Sasuke |
| 3 | Yae |

One companion stands about 78 world units behind the player. Two use left/right offsets at the same distance. With all four characters unlocked, the third follower occupies the center at about 128 units. Position and height are interpolated; a follower snaps back into formation after a warp or a displacement greater than 300 world units.

When the active character changes, the selected companion's slot is rebound to the previous active character without clearing that slot's position. `FUN_801DD5C0` writes the new character immediately but leaves the player in action `0xBA` while `FUN_801E0944` performs the deferred resource load. Team Up waits until action `0xBA` ends before committing the role handoff.

## Cutscene-model renderer

Ghidra confirms the renderer uses the same non-playable primitives as the game's scripted character models:

1. `func_80034E08_35A08` creates a plain child task.
2. `func_8000DBF0_E7F0` attaches one kind-2 model/display object.
3. The object receives position at `+0x08..+0x10`, rotations at `+0x14..+0x18`, scale at `+0x1C..+0x24`, animation frame at `+0x28`, model pointer at `+0x2C`, and segment bases from `+0x38` onward.

The four clothed character resource pairs are:

| Character | Broad file | Raw action-model file |
| --- | ---: | ---: |
| Goemon | `0x120` | `0x123` |
| Ebisumaru | `0x124` | `0x127` |
| Sasuke | `0x128` | `0x12B` |
| Yae | `0x12C` | `0x12F` |

The broad-file ids come from `D_80204020_5BFF30`; action-file ids come from `D_80204028_5BFF38`. Per-character `0x1C`-byte action records are selected through `D_80203F34_5BFE44`. Face and part resources are loaded into per-follower double buffers reserved inside the original 8 MiB RDRAM window, because their generated display lists cannot safely reference an extended `recomp_alloc` address.

Resource loading runs from a return hook on `func_8020D6BC_5C8B8C`, after the stage-specific resource loader. Follower snapshots are published from a return hook on the global game step `func_80002040_2C40`; each child task consumes its latest snapshot at the engine's normal pre-render update point.

No playable constructor, player-manager slot, controller callback, action behavior callback, collision actor, or story flag is installed for a companion.

## Current boundary

Followers are render-only. Their target formation does not run room collision or pathfinding, so they can visually pass through narrow geometry before interpolation or the distance snap catches up. They copy the active player's action and animation phase; this keeps the four character action tables synchronized but means a follower correcting its position while the player is idle may move briefly on an idle pose.

The main implementation files are:

- `src/team_up_followers.c`: unlock roster, formation motion, swap handoff, and hooks.
- `src/team_up_models.c`: resource staging, child-task lifecycle, model/action binding, face-resource sequencing, and rendering.
- `include/team_up_models.h`: three-slot renderer contract.
