#include "modding.h"
#include "common.h"
#include "recomputils.h"
#include "recompconfig.h"

// text_labels_add_ascii_entry
void func_800042AC_4EAC(s16 entry, const char* string, s16 x, s16 y);

// Hooks the game's mode handler function to display a string on the screen.
RECOMP_HOOK_RETURN("func_80002040_2C40") void test_hook() {
    // Use text slot 15 to display the string "THIS IS A TEST FROM MOD WORLD" at the screen coordinates (16, 16).
    func_800042AC_4EAC(15, "THIS IS A TEST FROM MOD WORLD", 16, 16);
}