# Team Up implementation

## Behavior

Team Up maintains three stable companion slots. On each gameplay frame it reads the four recruited-character values from `D_8015C608_15D208 + 0x94..0xA0`, excludes the active character in `0x8015C5DC`, and fills the remaining slots with unlocked characters in the game's stable order:

| ID | Character |
| ---: | --- |
| 0 | Goemon |
| 1 | Ebisumaru |
| 2 | Sasuke |
| 3 | Yae |

The followers use a compact formation 16 to 22 world units behind the player, with a 10-unit side offset for the first two slots. Those are destination points, not fixed transforms: every follower owns its own position, velocity, facing, and locomotion state.

The per-follower state machine has three states:

- `WALKING`: while close to the player, move toward the assigned trailing point at 1.25 units per frame, bind the game's walking action `0x0D`, and advance it by 0.75 animation frames per update. A settled follower binds idle action `0`.
- `RUNNING`: after distance from the player exceeds 32 units, move at 4.5 units per frame, bind the full-run action `0x0F`, and advance it by 1.70 frames per update. It returns to `WALKING` at 25 units, providing transition hysteresis.
- `JUMPING`: while Player 1's task action is `0x17`, `0x18`, `0x19`, or the continuing fall loop `0x1A`, apply Player 1's exact vertical displacement since takeoff and bind the corresponding airborne action for the follower's own character. The player's normalized animation phase is mapped to the follower clip, and the grounded `0x1B`/`0x1C` actions return the follower to `WALKING` or `RUNNING` based on distance.

Distance is measured in three dimensions. If it exceeds 50 units, the follower is immediately teleported to its assigned point behind the player and reset to `WALKING`, or kept in `JUMPING` while Player 1 is airborne. A second post-movement leash check makes 50 units a hard invariant rather than an approximate snap threshold.

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

Followers are render-only. Their movement state machine does not run room collision or pathfinding, so they can visually pass through narrow geometry before the 50-unit leash teleports them behind the player. They do not copy the active player's ordinary action, animation phase, or rotation. Ghidra shows `FUN_801E1C90` selecting the actual ground-locomotion actions `0x0C` through `0x0F` and `FUN_801E3400` binding that selection while the player moves. Team Up uses `0x0D` for walking, `0x0F` for running, and action `0` only when a companion has settled. Walking and running use explicit AI-owned frame steps rather than the playable task's dynamic rate adjustment.

The player task map used by the AI is:

| Address | Role |
| --- | --- |
| `0x801FC600` | Player Manager task pointer |
| `0x801FC604` | Player 1 task pointer; action byte at task `+0xCC` |
| `0x801FC60C` | Player 1 display object pointer; position at `+0x08..+0x10` |
| `0x801FC614` | Player 1 parent object pointer |

Ghidra shows `FUN_801E20A0` initiating a normal jump before action `0x17`, `FUN_801E8194` advancing through `0x18`/`0x19`, and `FUN_801DF234` selecting airborne fall loop `0x1A`. `FUN_801DF120` keeps `0x1A` active until the grounded transition to `0x1B`/`0x1C`. Team Up reads those actions plus Player 1's object height and animation phase, but never calls the player's behavior callbacks on a companion.

The main implementation files are:

- `src/team_up_followers.c`: unlock roster, formation motion, swap handoff, and hooks.
- `src/team_up_models.c`: resource staging, child-task lifecycle, model/action binding, face-resource sequencing, and rendering.
- `include/team_up_models.h`: three-slot renderer contract.
