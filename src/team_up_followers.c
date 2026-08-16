/**
 * @file team_up_followers.c
 * @brief Build and move the unlocked-character companion party.
 *
 * The companions are render-only cutscene tasks. They never receive a player
 * callback, collision body, controller input, damage state, or camera state.
 * The live player task is used only as their lifetime owner.
 */

#include "modding.h"
#include "recomputils.h"
#include "team_up_models.h"

#define CHARACTER_COUNT 4
#define FOLLOWER_COUNT_MAX 3
#define SAVE_CHARACTER_BASE 0x94
#define SAVE_HP_MAX_OFFSET (-0x28)
#define PLAYER_ACTION_CHARACTER_SWITCH 0xba

/* The three-character formation is a shallow V behind the player. Distances
 * are world units. Followers interpolate toward these points and snap only
 * after a warp, a large fall, or another discontinuity. */
#define FOLLOW_NEAR_BACK 78.0f
#define FOLLOW_FAR_BACK 128.0f
#define FOLLOW_SIDE 48.0f
#define FOLLOW_POSITION_FACTOR 0.20f
#define FOLLOW_HEIGHT_FACTOR 0.30f
#define FOLLOW_SETTLE_DISTANCE_SQ 1.0f
#define FOLLOW_SNAP_DISTANCE_SQ 90000.0f
#define FOLLOW_SNAP_HEIGHT 180.0f

typedef struct PlayerObject
{
    unsigned char header[8];
    float x;
    float y;
    float z;
} PlayerObject;

typedef struct FollowerMotion
{
    int initialized;
    float x;
    float y;
    float z;
    float previous_x;
    float previous_y;
    float previous_z;
} FollowerMotion;

/* Authoritative save block. The four recruited-character fields are signed
 * 32-bit values at +0x94, +0x98, +0x9c, and +0xa0. */
extern unsigned char D_8015C608_15D208[];

/* Live player task and its primary render object. Ghidra shows character at
 * task+0x60, action at task+0xcc, and the model transform at object+0x08. */
extern void *D_801FC604_5B8514;
extern PlayerObject *D_801FC60C_5B851C;

/* Current room and selected character. The room boundary also prevents
 * interpolation across a stage/room transition that reuses the same task. */
extern unsigned short D_800C7AB2;
#define CURRENT_CHARACTER (*(volatile unsigned int *)0x8015C5DC)

/* Resolve the selected local clip length without dereferencing its segmented
 * model pointer. The companions bind the same action id for their character. */
extern float func_8001B5AC_1C1AC(void *object);

#define SAVE_READ32(off) \
    (*(volatile signed int *)((unsigned char *)D_8015C608_15D208 + (off)))

static int s_slot_character[FOLLOWER_COUNT_MAX] = {-1, -1, -1};
static FollowerMotion s_motion[FOLLOWER_COUNT_MAX];
static void *s_owner_task;
static int s_last_active_character = -1;
static int s_last_room = -1;
static int s_sequence;

static unsigned char read_u8_at(const void *object, unsigned int offset)
{
    return *(const unsigned char *)((const unsigned char *)object + offset);
}

static short read_s16_at(const void *object, unsigned int offset)
{
    return *(const short *)((const unsigned char *)object + offset);
}

static float read_float_at(const void *object, unsigned int offset)
{
    return *(const float *)((const unsigned char *)object + offset);
}

static int is_rdram_pointer(const void *pointer)
{
    unsigned int address = (unsigned int)(unsigned long)pointer;
    unsigned int physical = address & 0x1fffffffu;

    return physical >= 0x00001000u && physical < 0x00800000u;
}

static int save_is_loaded(void)
{
    return SAVE_READ32(SAVE_HP_MAX_OFFSET) > 0;
}

static int character_is_unlocked(int character)
{
    if (character < 0 || character >= CHARACTER_COUNT)
        return 0;
    return SAVE_READ32(SAVE_CHARACTER_BASE + character * 4) != 0;
}

static int find_character_slot(int character)
{
    int slot;

    for (slot = 0; slot < FOLLOWER_COUNT_MAX; ++slot)
    {
        if (s_slot_character[slot] == character)
            return slot;
    }
    return -1;
}

static int first_empty_slot(void)
{
    int slot;

    for (slot = 0; slot < FOLLOWER_COUNT_MAX; ++slot)
    {
        if (s_slot_character[slot] < 0)
            return slot;
    }
    return -1;
}

static void clear_party_state(void)
{
    int slot;

    for (slot = 0; slot < FOLLOWER_COUNT_MAX; ++slot)
    {
        s_slot_character[slot] = -1;
        s_motion[slot].initialized = 0;
    }
    s_last_active_character = -1;
    s_sequence = 0;
}

static void add_missing_unlocked_characters(int active_character)
{
    int character;

    for (character = 0; character < CHARACTER_COUNT; ++character)
    {
        int slot;

        if (character == active_character || !character_is_unlocked(character) ||
            find_character_slot(character) >= 0)
            continue;
        slot = first_empty_slot();
        if (slot < 0)
            return;
        s_slot_character[slot] = character;
        s_motion[slot].initialized = 0;
        recomp_printf("[team_up] follower slot %d assigned character %d\n",
                      slot, character);
    }
}

static void remove_invalid_characters(int active_character)
{
    int slot;

    for (slot = 0; slot < FOLLOWER_COUNT_MAX; ++slot)
    {
        int character = s_slot_character[slot];

        if (character >= 0 &&
            (character == active_character || !character_is_unlocked(character)))
        {
            s_slot_character[slot] = -1;
            s_motion[slot].initialized = 0;
        }
    }
}

/* Reassign the selected follower's exact formation slot to the previous
 * player character. That is the visual handoff: the chosen NPC becomes the
 * player, and the character just left becomes the NPC in the same place. */
static void reconcile_party(int active_character, int player_action)
{
    int selected_slot;

    if (s_last_active_character < 0)
    {
        s_last_active_character = active_character;
        add_missing_unlocked_characters(active_character);
        return;
    }

    /* FUN_801DD5C0 updates the selected-character global before its deferred
     * resource swap finishes. Keep the existing roles until action 0xba ends
     * so the NPC changes at the same visible boundary as the player model. */
    if (active_character != s_last_active_character &&
        player_action != PLAYER_ACTION_CHARACTER_SWITCH)
    {
        selected_slot = find_character_slot(active_character);
        if (selected_slot >= 0 &&
            character_is_unlocked(s_last_active_character))
        {
            int previous_character = s_last_active_character;

            s_slot_character[selected_slot] = s_last_active_character;
            /* Preserve s_motion[selected_slot] to keep the old character in
             * the selected NPC's exact physical formation position. */
            recomp_printf("[team_up] player character %d took slot from %d; "
                          "character %d is now the follower\n",
                          active_character, selected_slot, previous_character);
        }
        s_last_active_character = active_character;
    }

    remove_invalid_characters(s_last_active_character);
    add_missing_unlocked_characters(s_last_active_character);
}

/* 16-way fixed-point direction table. MNSG yaw 0 faces -Z, 0x4000 faces +X,
 * 0x8000 faces +Z, and 0xc000 faces -X. Avoiding libm keeps the mod C-only
 * and uses the same coarse facing convention as the remote renderer. */
static const short s_forward_x[16] = {
    0, 392, 724, 946, 1024, 946, 724, 392,
    0, -392, -724, -946, -1024, -946, -724, -392};
static const short s_forward_z[16] = {
    -1024, -946, -724, -392, 0, 392, 724, 946,
    1024, 946, 724, 392, 0, -392, -724, -946};
static const short s_right_x[16] = {
    1024, 946, 724, 392, 0, -392, -724, -946,
    -1024, -946, -724, -392, 0, 392, 724, 946};
static const short s_right_z[16] = {
    0, 392, 724, 946, 1024, 946, 724, 392,
    0, -392, -724, -946, -1024, -946, -724, -392};

static int occupied_slot_count(void)
{
    int count = 0;
    int slot;

    for (slot = 0; slot < FOLLOWER_COUNT_MAX; ++slot)
    {
        if (s_slot_character[slot] >= 0)
            count++;
    }
    return count;
}

static int occupied_rank(int target_slot)
{
    int rank = 0;
    int slot;

    for (slot = 0; slot < target_slot; ++slot)
    {
        if (s_slot_character[slot] >= 0)
            rank++;
    }
    return rank;
}

static void formation_offset(int count, int rank, float *back, float *side)
{
    if (count <= 1)
    {
        *back = FOLLOW_NEAR_BACK;
        *side = 0.0f;
    }
    else if (count == 2)
    {
        *back = FOLLOW_NEAR_BACK;
        *side = rank == 0 ? -FOLLOW_SIDE : FOLLOW_SIDE;
    }
    else if (rank < 2)
    {
        *back = FOLLOW_NEAR_BACK;
        *side = rank == 0 ? -FOLLOW_SIDE : FOLLOW_SIDE;
    }
    else
    {
        *back = FOLLOW_FAR_BACK;
        *side = 0.0f;
    }
}

static void update_motion(FollowerMotion *motion,
                          float target_x, float target_y, float target_z)
{
    float dx;
    float dy;
    float dz;
    float horizontal_distance_sq;

    motion->previous_x = motion->x;
    motion->previous_y = motion->y;
    motion->previous_z = motion->z;
    if (!motion->initialized)
    {
        motion->x = target_x;
        motion->y = target_y;
        motion->z = target_z;
        motion->previous_x = target_x;
        motion->previous_y = target_y;
        motion->previous_z = target_z;
        motion->initialized = 1;
        return;
    }

    dx = target_x - motion->x;
    dy = target_y - motion->y;
    dz = target_z - motion->z;
    horizontal_distance_sq = dx * dx + dz * dz;
    if (horizontal_distance_sq > FOLLOW_SNAP_DISTANCE_SQ ||
        dy > FOLLOW_SNAP_HEIGHT || dy < -FOLLOW_SNAP_HEIGHT)
    {
        motion->x = target_x;
        motion->y = target_y;
        motion->z = target_z;
        return;
    }

    if (horizontal_distance_sq <= FOLLOW_SETTLE_DISTANCE_SQ)
    {
        motion->x = target_x;
        motion->z = target_z;
    }
    else
    {
        motion->x += dx * FOLLOW_POSITION_FACTOR;
        motion->z += dz * FOLLOW_POSITION_FACTOR;
    }
    motion->y += dy * FOLLOW_HEIGHT_FACTOR;
}

static int build_follower_models(TeamUpFollowerModel *models,
                                 const PlayerObject *player_object,
                                 const void *player_task)
{
    int count = occupied_slot_count();
    int model_count = 0;
    int action = (int)read_u8_at(player_task, 0xcc);
    int frame_100 = (int)(read_float_at(player_object, 0x28) * 100.0f);
    float frame_count = func_8001B5AC_1C1AC((void *)player_object);
    int frame_count_100 = frame_count > 0.0f ? (int)(frame_count * 100.0f) : 0;
    int rot_x = (int)read_s16_at(player_object, 0x14);
    int rot_y = (int)read_s16_at(player_object, 0x16);
    int rot_z = (int)read_s16_at(player_object, 0x18);
    unsigned int direction = ((unsigned short)rot_y >> 12) & 15u;
    int slot;

    for (slot = 0; slot < FOLLOWER_COUNT_MAX; ++slot)
    {
        TeamUpFollowerModel *model;
        FollowerMotion *motion;
        float back;
        float side;
        float target_x;
        float target_z;

        if (s_slot_character[slot] < 0)
            continue;
        formation_offset(count, occupied_rank(slot), &back, &side);
        target_x = player_object->x -
                   (float)s_forward_x[direction] * back / 1024.0f +
                   (float)s_right_x[direction] * side / 1024.0f;
        target_z = player_object->z -
                   (float)s_forward_z[direction] * back / 1024.0f +
                   (float)s_right_z[direction] * side / 1024.0f;

        motion = &s_motion[slot];
        update_motion(motion, target_x, player_object->y, target_z);
        model = &models[model_count++];
        model->cid = slot + 1;
        model->ch = s_slot_character[slot];
        model->x = (int)motion->x;
        model->y = (int)motion->y;
        model->z = (int)motion->z;
        model->vx = (int)((motion->x - motion->previous_x) * 60.0f);
        model->vy = (int)((motion->y - motion->previous_y) * 60.0f);
        model->vz = (int)((motion->z - motion->previous_z) * 60.0f);
        model->seq = ++s_sequence;
        model->action = action;
        model->anim_frame_100 = frame_100;
        model->anim_frame_count_100 = frame_count_100;
        model->rot_x = rot_x;
        model->rot_y = rot_y;
        model->rot_z = rot_z;
        model->same_team = 1;
    }
    return model_count;
}

/* Stage resources must be loaded from the stage-load path, never from the
 * frame hook. This is the gameplay-overlay wrapper around the stage-specific
 * resource loader confirmed in Ghidra. */
RECOMP_HOOK_RETURN("func_8020D6BC_5C8B8C")
void team_up_load_character_resources(void)
{
    team_up_models_load_resources();
}

/* Run after the normal game step. The child tasks consume this snapshot on
 * their next scheduled update, before the engine walks model records. */
RECOMP_HOOK_RETURN("func_80002040_2C40")
void team_up_update_followers(void)
{
    TeamUpFollowerModel models[FOLLOWER_COUNT_MAX];
    void *owner_task = D_801FC604_5B8514;
    PlayerObject *player_object = D_801FC60C_5B851C;
    int active_character;
    int player_action;
    int model_count;

    if (!is_rdram_pointer(owner_task))
    {
        team_up_models_reset();
        clear_party_state();
        s_owner_task = 0;
        s_last_room = -1;
        return;
    }

    if (!save_is_loaded() || !is_rdram_pointer(player_object))
    {
        team_up_models_update(0, 0, owner_task);
        clear_party_state();
        s_owner_task = owner_task;
        return;
    }

    if (s_owner_task != owner_task)
    {
        clear_party_state();
        s_owner_task = owner_task;
        s_last_room = (int)D_800C7AB2;
    }
    else if (s_last_room != (int)D_800C7AB2)
    {
        int slot;

        for (slot = 0; slot < FOLLOWER_COUNT_MAX; ++slot)
            s_motion[slot].initialized = 0;
        s_last_room = (int)D_800C7AB2;
    }

    active_character = (int)(CURRENT_CHARACTER & 3u);
    player_action = (int)read_u8_at(owner_task, 0xcc);
    reconcile_party(active_character, player_action);
    model_count = build_follower_models(models, player_object, owner_task);
    team_up_models_update(models, model_count, owner_task);
}
