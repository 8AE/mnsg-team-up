#ifndef TEAM_UP_MODELS_H
#define TEAM_UP_MODELS_H

#define TEAM_UP_MODEL_MAX 3

typedef struct TeamUpFollowerModel
{
    int cid;
    int ch;
    int x;
    int y;
    int z;
    int vx;
    int vy;
    int vz;
    int seq;
    int action;
    int anim_frame_100;
    int anim_frame_count_100;
    int rot_x;
    int rot_y;
    int rot_z;
    int same_team;
} TeamUpFollowerModel;

void team_up_models_update(const TeamUpFollowerModel *followers, int count,
                           void *render_parent_task);
void team_up_models_load_resources(void);
void team_up_models_reset(void);

#endif
