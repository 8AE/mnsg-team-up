/**
 * @file team_up_models.c
 * @brief Render unlocked companion characters with cutscene-style model tasks.
 *
 * The opening-cutscene trace shows the game does NOT use the player manager
 * or the slot-indexed staging chain to display character models in scripted
 * scenes. Instead it:
 *
 *   1. Loads whole files resident with FUN_80013B14 during the stage resource
 *      hook, never from a per-frame callback.
 *   2. Spawns plain kind-2 render objects with FUN_8000DBF0: model command
 *      pointer at object+0x2c, per-segment (file id, base) pairs starting at
 *      object+0x34, position/rotation/scale, all with no player task.
 *   3. Animates through object+0x28 (frame) and object+0x30 (anim state).
 *
 * This file uses that task/object architecture, then binds immutable Goemon,
 * Ebisumaru, Sasuke, and Yae render assets and action records so the clothed
 * model and the sender's exact action clip can be displayed. It never calls a
 * playable character constructor, playable action callback, or player-manager
 * update.
 * The action files are cached whole in mod memory; the broad character files
 * are registered by the normal scene resource loader. Face/part resources are
 * different: their pixels are referenced by generated N64 display-list
 * commands, so their per-slot double buffers are carved from the stock scene
 * arena below 0x80800000 instead of the extended recomp heap.
 *
 * Object segment binding (matches the player object layout):
 *   +0x38 action file base (segment 8; action model pointers are file-
 *         relative, so the base is simply the cached file)
 *   +0x3c broad file id / +0x40 broad file base (segment 9)
 *   +0x50 / +0x58 aux face/part resources (segments from D_80203FF0),
 *         loaded per action with FUN_800145B4 into double buffers
 *   +0x2c action record model pointer | 0x60000000
 */

#include "team_up_models.h"
#include "modding.h"
#include "recomputils.h"

#define REMOTE_PLAYER_ACTION_IDLE 0
#define REMOTE_PLAYER_ACTION_MAX 0xe8
#define REMOTE_PLAYER_ACTION_CHARACTER_SWITCH 0xba
#define PLAYER_MODEL_RENDER_SEGMENT 0x60000000u
#define CLOTHED_CHARACTER_ANIM_CONTEXT 0xc01fc680u
#define CLOTHED_CHARACTER_OBJECT_MODE 2u
#define REMOTE_YAW_SPEED_THRESHOLD_SQ 64
#define REMOTE_MODEL_SCALE 0.1f
#define REMOTE_FRAME_SNAP_THRESHOLD 4.0f
#define REMOTE_PACKET_FRAME_INTERVAL 6.0f

#define REMOTE_MODEL_SLOT_COUNT TEAM_UP_MODEL_MAX
#define CHARACTER_COUNT 4
#define AUX_BUFFER_SIZE 0x1000u
#define AUX_RESOURCE_COUNT 2
#define AUX_FLIP_COUNT 2
#define AUX_SEQUENCE_MAX_STEPS 32
#define AUX_RESOURCE_ID_LIMIT 0x8770u
#define BUFFER_ALIGN 16u
#define SCENE_RESOURCE_ENTRY_COUNT 48
#define RENDER_RDRAM_END 0x80800000u
#define AUX_ARENA_SIZE                                                     \
    (REMOTE_MODEL_SLOT_COUNT * AUX_RESOURCE_COUNT * AUX_FLIP_COUNT *       \
     AUX_BUFFER_SIZE)

typedef struct SceneResourceEntry
{
    unsigned short file_id;
    unsigned short padding;
    unsigned char *data;
} SceneResourceEntry;

typedef struct CharacterModelCache
{
    int ready;
    unsigned char *broad;       /* resident broad file from the scene registry */
    unsigned char *action;      /* whole action-model file, raw ROM image */
    unsigned int action_size;
} CharacterModelCache;

typedef struct RemoteModelSlot
{
    int active;
    int cid;
    int seen;
    int bound_ch;     /* character currently bound to the object, -1 if none */
    int bound_action; /* action currently bound, -1 if none */
    int last_seq;
    int last_remote_frame_100;
    int last_remote_frame_count_100;
    short yaw;
    float frame;
    float frame_step;
    void *task;
    void *object;
    unsigned char *aux_buffer[AUX_RESOURCE_COUNT][AUX_FLIP_COUNT];
    unsigned int aux_resource_id[AUX_RESOURCE_COUNT];
    int aux_flip[AUX_RESOURCE_COUNT];
    unsigned char aux_cursor[AUX_RESOURCE_COUNT];
    unsigned char aux_skip_update[AUX_RESOURCE_COUNT];
    float aux_last_frame;
    TeamUpFollowerModel pending_remote;
    int pending_valid;
} RemoteModelSlot;

/* Allocate and insert an engine task under `task_list` with an update
 * callback. Same allocator the opening cutscene uses for its scene tasks. */
extern void *func_80034E08_35A08(void *task_list, void (*update)(void *, void *),
                                 unsigned short flags);

/* Spawn a kind-2 render object under a task: sets object+0x2c (model command
 * pointer), +0x30 (anim state pointer), position/rotation/scale, the segment
 * file ids at +0x34/+0x3c, then registers segment bases via FUN_80014218.
 * This is the primitive every cutscene prop/character is spawned with. */
extern void *func_8000DBF0_E7F0(void *task, unsigned int model_ptr, unsigned int anim_ptr,
                                float x, float y, float z,
                                short rot_x, short rot_y, short rot_z,
                                float scale_x, float scale_y, float scale_z,
                                short seg8_file_id, short seg9_file_id);

/* Load and decompress a broad character resource into the game's scene
 * registry. This is called only by the stage-load return hook. */
extern void *func_80013B14_14714(unsigned int file_id);

/* Look up a file already resident in the scene registry. The returned base is
 * bound to segment 9 of each remote model object. */
extern void *func_800141C4_14DC4(unsigned int file_id);

/* Scene resource registry and resident-arena cursor. Ghidra shows
 * func_80013B14 using the first file_id == 0 entry's data pointer as the next
 * broad-resource destination; func_801DC630 carves the stock player's
 * persistent face buffers from this same arena. */
extern SceneResourceEntry D_80167FC0_168BC0[SCENE_RESOURCE_ENTRY_COUNT];

/* Blocking DMA copy from ROM. The action-model file is stored raw (the stock
 * func_801DC70C DMAs ranges of it directly), so the whole file is copied. */
extern void func_80001640_2240(unsigned int rom_addr, void *dst, unsigned int size);

/* ROM start / end address of a file id. */
extern unsigned int func_80001D68_2968(unsigned int file_id);
extern unsigned int func_80001D94_2994(unsigned int file_id);

/* Load a compressed resource by id into dst; returns end pointer. Used for
 * the per-action aux face/part resources (like func_801DC87C). */
extern unsigned char *func_800145B4_151B4(unsigned int resource_id, void *dst);

/* Return the stored size of a resource id without loading it. The remote
 * renderer uses this to reject invalid aux ids before they can write display
 * data outside a stock-sized face buffer. */
extern int func_80014698_15298(unsigned int resource_id, void *rom_address_out);

/* Frame count of the object's currently bound model; used to wrap the
 * remote animation frame the same way the stock player update does. */
extern float func_8001B5AC_1C1AC(void *object);

/* Immutable per-character action record arrays (0x1C-byte records): +0x00 model
 * command pointer, +0x04 anim speed *100, +0x0c/+0x10 model data range,
 * +0x14 aux selector byte, +0x18 aux resource table. */
extern unsigned char *D_80203F34_5BFE44[];

/* Aux staging descriptor bytes: two (?, segment) pairs; the segment bytes
 * pick the action-start object segment bases at +0x38 + segment*8. */
extern unsigned char D_80203FF0_5BFF00[];

/* Per-frame aux descriptors used by FUN_801DB060/FUN_801DB1D4. Each low byte
 * selects the object segment rebound when a timed face/part resource changes. */
extern unsigned short D_80203FF8_5BFF08[];

/* Per-character broad character-resource file ids. All four entries are read
 * to stage the correct clothed Goemon/Ebisumaru/Sasuke/Yae render data. */
extern unsigned short D_80204020_5BFF30[];

/* Per-character raw action-model file ids. All four entries are copied into
 * mod-owned render caches for exact remote action selection. */
extern unsigned short D_80204028_5BFF38[];

/* DMA mode byte the stock action copy (func_801DC70C) forces to 1 around
 * its ROM DMA and then restores; mirrored here for the whole-file copy.
 * Symbol D_8015C5D4_15D1D4 lives in the ABSOLUTE_SYMS pseudo-section, so it
 * is addressed directly. */
#define STOCK_DMA_MODE (*(volatile unsigned char *)0x8015C5D4)

static CharacterModelCache s_char_cache[CHARACTER_COUNT];
static RemoteModelSlot s_slots[REMOTE_MODEL_SLOT_COUNT];
static void *s_owner_task;
static unsigned char *s_aux_arena_next;
static unsigned char *s_aux_arena_end;

static void remote_model_task_update(void *task, void *object);

static void write_u8_at(void *obj, unsigned int offset, unsigned char value)
{
    *(unsigned char *)((unsigned char *)obj + offset) = value;
}

static void write_u16_at(void *obj, unsigned int offset, unsigned short value)
{
    *(unsigned short *)((unsigned char *)obj + offset) = value;
}

static void write_u32_at(void *obj, unsigned int offset, unsigned int value)
{
    *(unsigned int *)((unsigned char *)obj + offset) = value;
}

static void write_float_at(void *obj, unsigned int offset, float value)
{
    *(float *)((unsigned char *)obj + offset) = value;
}

static int is_rdram_pointer(const void *ptr)
{
    unsigned int addr = (unsigned int)(unsigned long)ptr;
    unsigned int phys = addr & 0x1fffffffu;

    /* Exclude the engine's 0x80000000 invalid-link sentinel as well as null.
     * Every task, object, and resident resource used here is above the first
     * RDRAM page. */
    return phys >= 0x00001000u && phys < 0x00800000u;
}

/* Ghidra: a live task's +0x04 field points to the list word that currently
 * references that task. FUN_80034A10 clears +0x04 when a task is freed, while
 * FUN_800350C4 assumes the backlink is live and splices through it. Validate
 * both halves before touching a retained remote child during owner teardown. */
static int is_linked_task(const void *task)
{
    void *backlink;

    if (!is_rdram_pointer(task))
        return 0;
    backlink = *(void *const *)((const unsigned char *)task + 0x04);
    if (!is_rdram_pointer(backlink))
        return 0;
    return *(void *const *)backlink == task;
}

/* A task-pool address can be reused for an unrelated task after teardown.
 * FUN_80034B58 stores the update callback at task+0x0C, so require Anchor's
 * callback in addition to valid list linkage before retaining a child. */
static int is_linked_remote_task(const void *task)
{
    if (!is_linked_task(task))
        return 0;
    return *(void *const *)((const unsigned char *)task + 0x0c) ==
           (void *)remote_model_task_update;
}

static unsigned char *alloc_aligned(unsigned int size)
{
    unsigned int addr = (unsigned int)(unsigned long)recomp_alloc(size + BUFFER_ALIGN);

    if (addr == 0)
        return 0;
    return (unsigned char *)(unsigned long)((addr + (BUFFER_ALIGN - 1u)) & ~(BUFFER_ALIGN - 1u));
}

static void invalidate_aux_render_arena(void)
{
    int slot_index;
    int channel;
    int flip;

    s_aux_arena_next = 0;
    s_aux_arena_end = 0;
    for (slot_index = 0; slot_index < REMOTE_MODEL_SLOT_COUNT; ++slot_index)
    {
        RemoteModelSlot *slot = &s_slots[slot_index];

        for (channel = 0; channel < AUX_RESOURCE_COUNT; ++channel)
        {
            for (flip = 0; flip < AUX_FLIP_COUNT; ++flip)
                slot->aux_buffer[channel][flip] = 0;
            slot->aux_resource_id[channel] = 0;
            slot->aux_flip[channel] = 0;
            slot->aux_cursor[channel] = 0;
            slot->aux_skip_update[channel] = 0;
        }
        /* Force the next scheduled task update to rebind segment 8/9 and both
         * face segments from the newly rebuilt external scene arena. */
        slot->bound_ch = -1;
        slot->bound_action = -1;
        slot->aux_last_frame = 0.0f;
    }
}

/* Reserve all renderer-visible face buffers from the stock resident resource
 * arena. FUN_801DC630 uses the same bump-allocation pattern for the local
 * player's four 0x1000-byte buffers. Keeping these addresses in the original
 * 8 MiB RDRAM window prevents N64 texture commands from truncating an extended
 * recomp_alloc address such as 0x81000000 to unrelated texture memory. */
static int reserve_aux_render_arena(void)
{
    SceneResourceEntry *free_entry = 0;
    unsigned int start;
    unsigned int end;
    int i;

    /* Read the external scene registry to find the loader's sentinel cursor;
     * reserving at that cursor keeps all already-loaded broad dependencies
     * below the remote face arena. */
    for (i = 0; i < SCENE_RESOURCE_ENTRY_COUNT; ++i)
    {
        if (D_80167FC0_168BC0[i].file_id == 0)
        {
            free_entry = &D_80167FC0_168BC0[i];
            break;
        }
    }
    if (!free_entry || !free_entry->data)
        return 0;

    start = ((unsigned int)(unsigned long)free_entry->data & 0xbfffffffu);
    start = (start + (BUFFER_ALIGN - 1u)) & ~(BUFFER_ALIGN - 1u);
    end = start + AUX_ARENA_SIZE;
    if (end < start || end > RENDER_RDRAM_END ||
        !is_rdram_pointer((void *)(unsigned long)start) ||
        !is_rdram_pointer((void *)(unsigned long)(end - 1u)))
    {
        recomp_printf("[team_up_models] face arena %x..%x is outside render RDRAM\n",
                      start, end);
        return 0;
    }

    /* Advance the external scene loader's sentinel because later resident
     * resources must begin after the remote face buffers, not overwrite them. */
    free_entry->data = (unsigned char *)(unsigned long)end;
    s_aux_arena_next = (unsigned char *)(unsigned long)start;
    s_aux_arena_end = (unsigned char *)(unsigned long)end;
    recomp_printf("[team_up_models] face arena reserved at %x..%x\n", start, end);
    return 1;
}

static unsigned char *alloc_aux_render_buffer(void)
{
    unsigned char *buffer;

    if (!s_aux_arena_next || !s_aux_arena_end ||
        s_aux_arena_next > s_aux_arena_end - AUX_BUFFER_SIZE)
        return 0;
    buffer = s_aux_arena_next;
    s_aux_arena_next += AUX_BUFFER_SIZE;
    return buffer;
}

/* ------------------------------------------------------------------ */
/* Character model cache                                              */
/* ------------------------------------------------------------------ */

static unsigned char *resident_resource_base(unsigned int file_id)
{
    void *resource;
    unsigned int address;

    /* Use the engine registry lookup because FUN_80013B14 owns the broad-file
     * allocation and may return a C0-tagged cached address. */
    resource = func_800141C4_14DC4(file_id);
    if (!resource || resource == (void *)(unsigned long)0xffffffffu)
        return 0;
    address = (unsigned int)(unsigned long)resource & 0xbfffffffu;
    if (!is_rdram_pointer((void *)(unsigned long)address))
        return 0;
    return (unsigned char *)(unsigned long)address;
}

static int cache_action_file(int ch)
{
    CharacterModelCache *cache = &s_char_cache[ch];
    /* Use the immutable file-id table to select only this remote character's
     * raw display data, without asking a player task to stage an action. */
    unsigned int action_id = D_80204028_5BFF38[ch];
    unsigned int file_start;
    unsigned int file_end;
    unsigned char saved_dma_mode;

    if (cache->action)
        return 1;

    /* Read the immutable action file's ROM bounds so the entire file can be
     * cached once and shared by every cutscene-style remote of this character. */
    file_start = func_80001D68_2968(action_id);
    file_end = func_80001D94_2994(action_id);
    if (file_end <= file_start)
    {
        recomp_printf("[team_up_models] ch %d action file %x bad range\n",
                      ch, action_id);
        return 0;
    }

    cache->action_size = file_end - file_start;
    cache->action = alloc_aligned(cache->action_size);
    if (!cache->action)
        return 0;

    /* Match the stock action DMA's synchronous mode so model data is complete
     * before a remote object can bind a pointer into this cache. */
    saved_dma_mode = STOCK_DMA_MODE;
    STOCK_DMA_MODE = 1;
    func_80001640_2240(file_start, cache->action, cache->action_size);
    STOCK_DMA_MODE = saved_dma_mode;
    return 1;
}

void team_up_models_load_resources(void)
{
    int ch;

    /* A new stage rebuilds the external scene registry and its resident arena,
     * so discard every pointer into the previous stage before reserving again. */
    invalidate_aux_render_arena();

    for (ch = 0; ch < CHARACTER_COUNT; ++ch)
    {
        CharacterModelCache *cache = &s_char_cache[ch];
        /* Use the immutable broad-file table to keep all four character
         * resources independent of the local player's selected character. */
        unsigned int broad_id = D_80204020_5BFF30[ch];

        cache->ready = 0;
        cache->broad = 0;

        /* Use the scene loader during the stage-load return hook so every
         * character's clothed broad render resources are resident before any
         * per-frame remote task runs. */
        func_80013B14_14714(broad_id);
        cache->broad = resident_resource_base(broad_id);
        if (!cache->broad || !cache_action_file(ch))
        {
            recomp_printf("[team_up_models] ch %d resource staging failed\n", ch);
            continue;
        }

        cache->ready = 1;
        recomp_printf("[team_up_models] ch %d clothed model/action cache ready\n",
                      ch);
    }

    if (!reserve_aux_render_arena())
    {
        /* Without original-RDRAM face storage, leave all external character
         * resources unavailable instead of submitting corrupt texture data. */
        for (ch = 0; ch < CHARACTER_COUNT; ++ch)
            s_char_cache[ch].ready = 0;
        recomp_printf("[team_up_models] face arena reservation failed\n");
    }
}

/* ------------------------------------------------------------------ */
/* Remote model slots                                                 */
/* ------------------------------------------------------------------ */

static void hide_object(void *object)
{
    if (!object)
        return;

    write_u32_at(object, 0x2c, 0);
    write_float_at(object, 0x1c, 0.0f);
    write_float_at(object, 0x20, 0.0f);
    write_float_at(object, 0x24, 0.0f);
    write_u8_at(object, 0x65, 1);
}

static void show_object(void *object)
{
    float scale = REMOTE_MODEL_SCALE;

    write_float_at(object, 0x1c, scale);
    write_float_at(object, 0x20, scale);
    write_float_at(object, 0x24, scale);
    write_u8_at(object, 0x65, 0);
}

/* Retire a peer without directly destroying its engine task. Remote children
 * belong to the live player-owner tree, so the engine reclaims them exactly
 * once when that owner is destroyed. While the owner remains linked, keep the
 * hidden task/object pair for reuse by the next peer assigned to this slot. */
static void clear_slot_state(RemoteModelSlot *slot, int preserve_live_task)
{
    void *retained_task = 0;
    void *retained_object = 0;
    int i;

    if (preserve_live_task && is_linked_remote_task(slot->task))
    {
        retained_task = slot->task;
        if (is_rdram_pointer(slot->object))
        {
            hide_object(slot->object);
            retained_object = slot->object;
        }
    }

    slot->active = 0;
    slot->cid = 0;
    slot->seen = 0;
    slot->bound_ch = -1;
    slot->bound_action = -1;
    slot->last_seq = 0;
    slot->last_remote_frame_100 = 0;
    slot->last_remote_frame_count_100 = 0;
    slot->yaw = 0;
    slot->frame = 0.0f;
    slot->frame_step = 1.0f;
    slot->aux_last_frame = 0.0f;
    slot->pending_valid = 0;
    slot->task = retained_task;
    slot->object = retained_object;
    for (i = 0; i < AUX_RESOURCE_COUNT; ++i)
    {
        slot->aux_resource_id[i] = 0;
        slot->aux_cursor[i] = 0;
        slot->aux_skip_update[i] = 0;
    }
}

void team_up_models_reset(void)
{
    int i;

    for (i = 0; i < REMOTE_MODEL_SLOT_COUNT; ++i)
        clear_slot_state(&s_slots[i], 0);
    s_owner_task = 0;
}

static RemoteModelSlot *find_slot(int cid)
{
    int i;

    for (i = 0; i < REMOTE_MODEL_SLOT_COUNT; ++i)
    {
        if (s_slots[i].active && s_slots[i].cid == cid)
            return &s_slots[i];
    }
    return 0;
}

static RemoteModelSlot *alloc_slot(int cid)
{
    int i;

    for (i = 0; i < REMOTE_MODEL_SLOT_COUNT; ++i)
    {
        if (!s_slots[i].active)
        {
            /* Reuse a hidden task/object retained under the same live owner;
             * this bounds the task count to the fixed remote slot count. */
            clear_slot_state(&s_slots[i], 1);
            s_slots[i].active = 1;
            s_slots[i].cid = cid;
            return &s_slots[i];
        }
    }
    return 0;
}

static int ensure_slot_task(RemoteModelSlot *slot, const TeamUpFollowerModel *remote,
                            void *render_parent_task)
{
    float scale;
    void *object;

    /* If the task pool reused this address across an owner transition, discard
     * both stale handles before any object write or callback reuse. */
    if (slot->task && !is_linked_remote_task(slot->task))
    {
        slot->task = 0;
        slot->object = 0;
        slot->bound_ch = -1;
        slot->bound_action = -1;
    }
    if (slot->task && slot->object)
        return 1;

    if (!slot->task)
    {
        /* Use the external cutscene task allocator only when this owner-scoped
         * slot has no retained child task to reuse. */
        slot->task = func_80034E08_35A08(render_parent_task,
                                         remote_model_task_update, 0);
    }
    if (!slot->task)
        return 0;

    scale = REMOTE_MODEL_SCALE;

    /* Spawn hidden: model pointer 0 renders nothing. +0x30 gets the immutable
     * renderer context required by the clothed four-character display data;
     * no player initialization or behavior function is called. */
    object = func_8000DBF0_E7F0(slot->task, 0,
                                CLOTHED_CHARACTER_ANIM_CONTEXT,
                                (float)remote->x, (float)remote->y, (float)remote->z,
                                0, slot->yaw, 0,
                                scale, scale, scale,
                                0, 0);
    if (!object)
    {
        /* Keep the linked task and retry object allocation next frame. The
         * parent owner will reclaim it if the scene tears down meanwhile. */
        return 0;
    }

    /* Ghidra: FUN_801CC30C writes object+0x05 = 2 immediately after assigning
     * the clothed player animation context. FUN_8000DBF0 and its free-list
     * allocator do not initialize this byte, so explicitly select the same
     * render-object mode before binding the selected character display data.
     * This is renderer state only; no playable task or player behavior is
     * invoked. */
    write_u8_at(object, 0x05, CLOTHED_CHARACTER_OBJECT_MODE);
    slot->object = object;
    hide_object(object);
    return 1;
}

static unsigned char *get_action_entry(int ch, int action)
{
    if (ch < 0 || ch >= CHARACTER_COUNT || action < 0 || action >= REMOTE_PLAYER_ACTION_MAX)
        return 0;
    /* Read only the model metadata record matching the received character and
     * action; its associated playable callback table is never accessed. */
    return D_80203F34_5BFE44[ch] + action * 0x1c;
}

static int remote_action_or_idle(int action)
{
    if (action >= 0 &&
        action < REMOTE_PLAYER_ACTION_MAX &&
        action != REMOTE_PLAYER_ACTION_CHARACTER_SWITCH)
    {
        return action;
    }
    return REMOTE_PLAYER_ACTION_IDLE;
}

static int bind_aux_resource(RemoteModelSlot *slot, int channel,
                             unsigned int segment, unsigned int resource)
{
    unsigned char *buffer;
    unsigned char *end;
    int flip;

    if (resource == 0)
        return 1;
    if (channel < 0 || channel >= AUX_RESOURCE_COUNT ||
        segment < 1 || segment > 5 || resource == 0xffu ||
        resource >= AUX_RESOURCE_ID_LIMIT)
        return 0;

    if (slot->aux_resource_id[channel] == resource)
    {
        buffer = slot->aux_buffer[channel][slot->aux_flip[channel]];
        if (!buffer)
            return 0;
        write_u32_at(slot->object, 0x38 + segment * 8,
                     (unsigned int)(unsigned long)buffer);
        return 1;
    }

    /* Use the stock resource-size query to reject an unrelated table value
     * before FUN_800145B4 is allowed to write into a face buffer. Correct
     * player aux resources fit the 0x1000-byte buffers from FUN_801DC630. */
    {
        int stored_size = func_80014698_15298(resource, 0);

        if (stored_size <= 0 || stored_size > (int)AUX_BUFFER_SIZE)
        {
            recomp_printf("[team_up_models] invalid aux resource %x size %d\n",
                          resource, stored_size);
            return 0;
        }
    }

    /* Match FUN_801DC87C's double buffer so a display list can finish using
     * the previous expression while the next face/part resource is loaded. */
    flip = slot->aux_flip[channel] ^ 1;
    if (!slot->aux_buffer[channel][flip])
        slot->aux_buffer[channel][flip] = alloc_aux_render_buffer();
    buffer = slot->aux_buffer[channel][flip];
    if (!buffer)
        return 0;

    /* Use the stock small-resource loader because face resources may be raw or
     * compressed; validate its end pointer before exposing the segment base. */
    end = func_800145B4_151B4(resource, buffer);
    if (!end || end < buffer || end > buffer + AUX_BUFFER_SIZE)
    {
        recomp_printf("[team_up_models] aux resource %x overflow\n", resource);
        return 0;
    }
    slot->aux_flip[channel] = flip;
    slot->aux_resource_id[channel] = resource;
    write_u32_at(slot->object, 0x38 + segment * 8,
                 (unsigned int)(unsigned long)buffer);
    return 1;
}

/* Stage the two initial face/part resources exactly as FUN_801DAF54 does when
 * a new action is bound. Timed expression updates are handled separately. */
static int bind_initial_aux_resources(RemoteModelSlot *slot,
                                      unsigned char *entry)
{
    unsigned char *aux_table = *(unsigned char **)(entry + 0x18);
    int channel;

    if (!aux_table)
        return 1;
    for (channel = 0; channel < AUX_RESOURCE_COUNT; ++channel)
    {
        /* Mirror FUN_801DB180: channel zero begins at row zero; channel one
         * begins at the action record's +0x14 row. */
        unsigned int row = channel == 0 ? 0u : (unsigned int)*(entry + 0x14);
        unsigned int segment = D_80203FF0_5BFF00[channel * 2 + 1];
        unsigned int resource = *(unsigned int *)(aux_table + row * 8);

        if (!bind_aux_resource(slot, channel, segment, resource))
            return 0;
        slot->aux_cursor[channel] = 0;
        /* FUN_801DAF54 marks each channel so FUN_801DB060 skips exactly the
         * first per-frame update after an action-start resource bind. */
        slot->aux_skip_update[channel] = 1;
    }
    return 1;
}

/* Advance the two face/part cursors at most one row per rendered frame, as
 * FUN_801DB060/FUN_801DB1D4 do. Keeping cursor state is important when a peer
 * joins mid-animation: a stateless multi-row scan can leave the known action
 * sequence and submit an unrelated resource as an RT64 display list. */
static int sync_timed_aux_resources(RemoteModelSlot *slot,
                                    unsigned char *entry, float frame)
{
    unsigned char *aux_table = *(unsigned char **)(entry + 0x18);
    int rewound = frame < slot->aux_last_frame;
    int channel;

    if (!aux_table)
        return 1;
    for (channel = 0; channel < AUX_RESOURCE_COUNT; ++channel)
    {
        unsigned int base_row =
            channel == 0 ? 0u : (unsigned int)*(entry + 0x14);
        unsigned int cursor;
        unsigned int row;
        unsigned int resource;
        unsigned int segment;
        unsigned int threshold;

        if (slot->aux_skip_update[channel])
        {
            slot->aux_skip_update[channel] = 0;
            continue;
        }

        if (frame == 0.0f || rewound)
            slot->aux_cursor[channel] = 0;
        cursor = slot->aux_cursor[channel];
        if (cursor >= AUX_SEQUENCE_MAX_STEPS)
            return 0;

        row = base_row + cursor;
        threshold = *(aux_table + row * 8 + 4);
        if (frame != 0.0f && !rewound && (float)threshold > frame)
            continue;

        /* Stock advances no more than one record in a call, and a zero
         * threshold keeps the current record selected. */
        if (frame != 0.0f && !rewound && threshold != 0)
        {
            cursor++;
            if (cursor >= AUX_SEQUENCE_MAX_STEPS)
                return 0;
            slot->aux_cursor[channel] = (unsigned char)cursor;
            row = base_row + cursor;
        }

        resource = *(unsigned int *)(aux_table + row * 8);
        if (resource == 0xffu)
        {
            slot->aux_cursor[channel] = 0;
            resource = *(unsigned int *)(aux_table + base_row * 8);
        }
        /* Use the stock per-frame descriptor array because it can differ from
         * the action-start segment descriptor used by FUN_801DAF54. */
        segment = (unsigned int)(D_80203FF8_5BFF08[channel] & 0xffu);
        if (!bind_aux_resource(slot, channel, segment, resource))
            return 0;
    }

    slot->aux_last_frame = frame;
    return 1;
}

/* Bind a character/action to the slot's render object from the character
 * cache. Pure pointer rebinding; no data is copied except aux resources on
 * their first use. */
static int bind_model(RemoteModelSlot *slot, int ch, int action)
{
    CharacterModelCache *cache = &s_char_cache[ch];
    unsigned char *entry = get_action_entry(ch, action);
    short speed;

    if (!entry || !cache->ready || !cache->broad || !cache->action)
        return 0;

    /* Segment 8: action model file base. The stock copy sets
     * object+0x38 = buffer - (range_start - 0x7000000); with the whole file
     * cached, that base is simply the file itself. */
    write_u32_at(slot->object, 0x38, (unsigned int)(unsigned long)cache->action);
    /* Segment 9: broad character file id + base (object+0x3c/+0x40). */
    write_u16_at(slot->object, 0x3c, D_80204020_5BFF30[ch]);
    write_u32_at(slot->object, 0x40, (unsigned int)(unsigned long)cache->broad);

    if (!bind_initial_aux_resources(slot, entry))
        return 0;

    write_u32_at(slot->object, 0x2c,
                 *(unsigned int *)(entry + 0x00) + PLAYER_MODEL_RENDER_SEGMENT);

    speed = *(short *)(entry + 0x04);
    slot->frame_step = (float)speed / 100.0f;
    slot->frame = 0.0f;
    slot->aux_last_frame = 0.0f;
    write_float_at(slot->object, 0x28, 0.0f);

    slot->bound_ch = ch;
    slot->bound_action = action;
    show_object(slot->object);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Pose / animation                                                   */
/* ------------------------------------------------------------------ */

static short yaw_from_velocity(int vx, int vz, short fallback)
{
    int avx;
    int avz;

    if (vx * vx + vz * vz < REMOTE_YAW_SPEED_THRESHOLD_SQ)
        return fallback;

    avx = vx < 0 ? -vx : vx;
    avz = vz < 0 ? -vz : vz;

    if (avx > avz * 2)
        return vx >= 0 ? 0x4000 : (short)0xc000;
    if (avz > avx * 2)
        return vz >= 0 ? (short)0x8000 : 0;
    if (vx >= 0 && vz >= 0)
        return 0x6000;
    if (vx < 0 && vz >= 0)
        return (short)0xa000;
    if (vx < 0 && vz < 0)
        return (short)0xe000;
    return 0x2000;
}

static float frame_delta(float target, float current, float frame_count)
{
    float delta = target - current;

    if (frame_count > 1.0f)
    {
        float half = frame_count * 0.5f;

        if (delta > half)
            delta -= frame_count;
        else if (delta < -half)
            delta += frame_count;
    }
    return delta;
}

static float abs_float(float value)
{
    return value < 0.0f ? -value : value;
}

static void update_slot_pose(RemoteModelSlot *slot, const TeamUpFollowerModel *remote,
                             int action_changed)
{
    unsigned char *entry;
    float frame_count;
    int new_remote_packet = remote->seq != slot->last_seq;
    int use_remote_anim = remote->sync_animation &&
                          remote->action == slot->bound_action &&
                          remote->action >= 0 &&
                          remote->action < REMOTE_PLAYER_ACTION_MAX;
    int use_remote_rotation = remote->sync_rotation;

    if (!use_remote_rotation)
        slot->yaw = yaw_from_velocity(remote->vx, remote->vz, slot->yaw);

    entry = get_action_entry(slot->bound_ch, slot->bound_action);
    if (!entry)
    {
        hide_object(slot->object);
        slot->bound_action = -1;
        return;
    }

    if (use_remote_anim)
    {
        slot->frame += slot->frame_step;
    }
    else
    {
        int animation_step_100 = remote->animation_step_100;

        if (animation_step_100 < 0)
            animation_step_100 = 0;
        else if (animation_step_100 > 300)
            animation_step_100 = 300;

        /* AI locomotion supplies an absolute frames-per-update step instead
         * of depending on the playable task's dynamic rate adjustment. */
        slot->frame_step = (float)animation_step_100 / 100.0f;
        if (animation_step_100 == 0)
            slot->frame = 0.0f;
        else
            slot->frame += slot->frame_step;
    }
    /* Resolve the bound clip length through the engine because its model
     * command pointer is segmented and cannot be dereferenced directly. */
    frame_count = func_8001B5AC_1C1AC(slot->object);

    if (use_remote_anim && new_remote_packet)
    {
        float target_frame = (float)remote->anim_frame_100 / 100.0f;
        float source_frame_count =
            (float)remote->anim_frame_count_100 / 100.0f;

        /* The sender and receiver use the same character/action record. The
         * normalized fallback only protects against a resolver count mismatch. */
        if (source_frame_count > 1.0f && frame_count > 1.0f &&
            abs_float(source_frame_count - frame_count) >= 0.01f)
        {
            target_frame = target_frame / source_frame_count * frame_count;
        }

        if (!action_changed && slot->last_seq != 0 &&
            remote->anim_frame_count_100 > 100 &&
            remote->anim_frame_count_100 == slot->last_remote_frame_count_100)
        {
            float previous_frame =
                (float)slot->last_remote_frame_100 / 100.0f;
            float delta;

            if (source_frame_count > 1.0f && frame_count > 1.0f &&
                abs_float(source_frame_count - frame_count) >= 0.01f)
                previous_frame = previous_frame / source_frame_count * frame_count;
            delta = frame_delta(target_frame, previous_frame, frame_count);
            if (abs_float(delta) < REMOTE_FRAME_SNAP_THRESHOLD)
                slot->frame_step = delta / REMOTE_PACKET_FRAME_INTERVAL;
        }

        /* Snap to every authoritative packet. The derived step above predicts
         * the frames until the next expected packet, including paused clips. */
        slot->frame = target_frame;
        slot->last_remote_frame_100 = remote->anim_frame_100;
        slot->last_remote_frame_count_100 = remote->anim_frame_count_100;
    }

    if (frame_count > 1.0f)
    {
        while (slot->frame >= frame_count)
            slot->frame -= frame_count;
        while (slot->frame < 0.0f)
            slot->frame += frame_count;
    }
    else
    {
        slot->frame = 0.0f;
    }

    if (!sync_timed_aux_resources(slot, entry, slot->frame))
    {
        /* Keep a model with incomplete face segments hidden so corrupt display
         * data is never submitted while an aux load or table check fails. */
        hide_object(slot->object);
        slot->bound_action = -1;
        return;
    }

    write_float_at(slot->object, 0x08, (float)remote->x);
    write_float_at(slot->object, 0x0c, (float)remote->y);
    write_float_at(slot->object, 0x10, (float)remote->z);
    if (use_remote_rotation)
    {
        write_u16_at(slot->object, 0x14, (unsigned short)remote->rot_x);
        write_u16_at(slot->object, 0x16, (unsigned short)remote->rot_y);
        write_u16_at(slot->object, 0x18, (unsigned short)remote->rot_z);
        slot->yaw = (short)remote->rot_y;
    }
    else
    {
        write_u16_at(slot->object, 0x14, 0);
        write_u16_at(slot->object, 0x16, (unsigned short)slot->yaw);
        write_u16_at(slot->object, 0x18, 0);
    }
    write_float_at(slot->object, 0x28, slot->frame);
    slot->last_seq = remote->seq;
}

static void update_slot_hidden_pose(RemoteModelSlot *slot, const TeamUpFollowerModel *remote)
{
    if (!slot->object)
        return;

    write_float_at(slot->object, 0x08, (float)remote->x);
    write_float_at(slot->object, 0x0c, (float)remote->y);
    write_float_at(slot->object, 0x10, (float)remote->z);
}

/* Apply the queued network snapshot from the cutscene-style child task's
 * scheduled update. This matches the stock ordering: model pointers and aux
 * face memory are finalized before the engine walks kind-2 records to build
 * the frame's display list. The frame-end hook only publishes snapshots. */
static void remote_model_task_update(void *task, void *object)
{
    RemoteModelSlot *slot = 0;
    const TeamUpFollowerModel *remote;
    int ch;
    int action;
    int i;

    (void)object;
    for (i = 0; i < REMOTE_MODEL_SLOT_COUNT; ++i)
    {
        if (s_slots[i].active && s_slots[i].task == task)
        {
            slot = &s_slots[i];
            break;
        }
    }
    if (!slot || !slot->object || !slot->pending_valid)
        return;

    remote = &slot->pending_remote;
    ch = remote->ch;
    if (remote->cid <= 0 || ch < 0 || ch >= CHARACTER_COUNT)
    {
        hide_object(slot->object);
        slot->bound_ch = -1;
        slot->bound_action = -1;
        return;
    }

    action = remote_action_or_idle(remote->action);
    if (!s_char_cache[ch].ready)
    {
        if (slot->bound_ch != ch)
        {
            hide_object(slot->object);
            slot->bound_ch = -1;
            slot->bound_action = -1;
        }
        update_slot_hidden_pose(slot, remote);
        return;
    }

    if (slot->bound_ch != ch || slot->bound_action != action)
    {
        if (!bind_model(slot, ch, action))
        {
            hide_object(slot->object);
            slot->bound_ch = -1;
            slot->bound_action = -1;
            update_slot_hidden_pose(slot, remote);
            return;
        }
        update_slot_pose(slot, remote, 1);
        return;
    }

    update_slot_pose(slot, remote, 0);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

void team_up_models_update(const TeamUpFollowerModel *remotes, int count,
                                 void *render_parent_task)
{
    int i;

    if (!is_linked_task(render_parent_task))
    {
        team_up_models_reset();
        return;
    }

    if (s_owner_task != render_parent_task)
    {
        team_up_models_reset();
        s_owner_task = render_parent_task;
    }

    for (i = 0; i < REMOTE_MODEL_SLOT_COUNT; ++i)
        s_slots[i].seen = 0;

    for (i = 0; remotes && i < count; ++i)
    {
        const TeamUpFollowerModel *remote = &remotes[i];
        RemoteModelSlot *slot;

        if (remote->cid <= 0 || remote->ch < 0 || remote->ch >= CHARACTER_COUNT)
            continue;

        slot = find_slot(remote->cid);
        if (!slot)
            slot = alloc_slot(remote->cid);
        if (!slot)
            continue; /* more remotes than model slots; nameplate only */

        slot->seen = 1;
        if (!ensure_slot_task(slot, remote, render_parent_task))
            continue;
        /* Queue only plain network state here. The task callback consumes the
         * newest complete snapshot at the engine's safe pre-render point. */
        slot->pending_remote = *remote;
        slot->pending_valid = 1;
    }

    for (i = 0; i < REMOTE_MODEL_SLOT_COUNT; ++i)
    {
        if (s_slots[i].active && !s_slots[i].seen)
            clear_slot_state(&s_slots[i], 1);
    }
}
