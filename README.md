# Team Up

Team Up is a mod for Mystical Ninja Starring Goemon: Recompiled. This repository is based on the MNSG recomp mod template and is configured to produce `mnsg_team_up.nrm`.

The mod renders every unlocked character other than the currently controlled character as a cutscene-style AI companion. Goemon, Ebisumaru, Sasuke, and Yae use their clothed gameplay models and independently follow behind the player.

When the player changes character, Team Up preserves the selected companion's formation slot and rebinds that slot to the previous playable character. The handoff waits for the game's deferred character-load action (`0xBA`) to finish, avoiding a duplicate model during the transition.

Companions are visual followers, not additional playable actors. They do not receive controller input, collide, fight, take damage, drive the camera, or modify story state.

### Implementation

- Character availability is read from the four 32-bit save fields at offsets `0x94`, `0x98`, `0x9C`, and `0xA0`.
- Broad character resources and raw action-model files are staged from the gameplay stage-load hook.
- Each follower is an independent kind-2 model object under a plain child task, matching the game's cutscene actor architecture.
- Each follower owns a `WALKING`/`RUNNING`/`JUMPING` state machine, movement speed, facing, and animation frame step.
- Followers walk while close, run to catch up, and teleport into formation behind the player if they exceed the hard 50-unit leash.
- Moving companions bind the game's real walking/running action clips; idle action `0` is used only after they settle.
- During Player 1's complete airborne sequence, companions match the player's vertical displacement and normalized jump-animation phase with their own character clips.
- The implementation is entirely C and uses no Python runtime or native library.

See [docs/implementation.md](docs/implementation.md) for the Ghidra-backed model/resource path, lifecycle rules, and current render-only boundary.

### Writing mods
See [this document](https://hackmd.io/fMDiGEJ9TBSjomuZZOgzNg) for an explanation of the modding framework, including how to write function patches and perform interop between different mods.

### Tools
You'll need to install `clang` and `make` to build this mod.
* On Windows, using [chocolatey](https://chocolatey.org/) to install both is recommended. The packages are `llvm` and `make` respectively.
  * The LLVM 19.1.0 [llvm-project](https://github.com/llvm/llvm-project) release binary, which is also what chocolatey provides, does not support MIPS correctly. The solution is to install 18.1.8 instead, which can be done in chocolatey by specifying `--version 18.1.8` or by downloading the 18.1.8 release directly.
* On Linux, these can both be installed using your distro's package manager. You may also need to install your distro's package for the `lld` linker. On Debian/Ubuntu based distros this will be the `lld` package.
* On MacOS, these can both be installed using Homebrew. Apple clang won't work, as you need a mips target for building the mod code.

On Linux and MacOS, you'll need to also ensure that you have the `zip` utility installed.

You'll also need to grab a build of the `RecompModTool` utility from the releases of [N64Recomp](https://github.com/N64Recomp/N64Recomp). You can also build it yourself from that repo if desired.

### Building

On macOS, install the build dependencies with Homebrew:

```sh
brew install llvm lld make
```

Place a macOS build of `RecompModTool` in the repository root, put it on your `PATH`, or set `RECOMP_MOD_TOOL` to its path. This checkout also reuses `../mnsg-recomp-example/RecompModTool` when that configured sibling repository is present.

Build the complete `.nrm` package with:

```sh
./build_mod.sh -j4
```

The script selects Homebrew's MIPS-capable LLVM toolchain, compiles the mod, packages it, and writes `build/mnsg_team_up.nrm`.

### Updating the Mystical Ninja Starring Goemon Decompilation Submodule
Mods can also be made with newer versions of the Mystical Ninja Starring Goemon decompilation instead of the commit targeted by this repo's submodule.
To update the commit of the decompilation that you're targeting, follow these steps:
* Build the [N64Recomp](https://github.com/N64Recomp/N64Recomp) repo and copy the N64Recomp executable to the root of this repository.
* Build the version of the Mystical Ninja Starring Goemon decompilation that you want to update to and copy the resulting .elf file to the root of this repository.
* Update the `mnsg` submodule in your clone of this repo to point to the commit you built in the previous step.
* Run `N64Recomp generate_symbols.toml --dump-context`
* Rename `dump.toml` and `data_dump.toml` to `mnsg.us.syms.toml` and `mnsg.us.datasyms.toml` respectively.
  * Place both files in the `Goemon64RecompSyms` folder.
* Try building.
  * If it succeeds, you're done.
  * If it fails due to a missing header, create an empty header file in the `include/dummy_headers` folder, with the same path.
    * For example, if it complains that `assets/objects/object_cow/object_cow.h` is missing, create an empty `include/dummy_headers/objects/object_cow.h` file.
  * If RecompModTool fails due to a function "being marked as a patch but not existing in the original ROM", it's likely that function you're patching was renamed in the Mystical Ninja Starring Goemon decompilation.
    * Find the relevant function in the map file for the old decomp commit, then go to that address in the new map file, and update the reference to this function in your code with the new name.
