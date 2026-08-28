#ifndef LAYOUT_H
#define LAYOUT_H

#include "view.h"

typedef enum {
    SPLIT_HORIZONTAL,
    SPLIT_VERTICAL
} SplitDirection;

typedef struct DwindleNode {
    struct DwindleNode *parent;
    struct DwindleNode *children[2];
    View *view;

    SplitDirection split;
    double split_ratio;

    int x;
    int y;
    int width;
    int height;
} DwindleNode;

typedef struct {
    DwindleNode *root;
    DwindleNode *focused;

    int width;
    int height;
    int origin_x;
    int origin_y;

    int master_count;
    double master_factor;

    int gaps_in;
    int gaps_out;
} DwindleLayout;

void dwindle_init(DwindleLayout *layout);
void dwindle_destroy(DwindleLayout *layout);
void dwindle_set_gaps(DwindleLayout *layout, int gaps_in, int gaps_out);
void dwindle_add_view(DwindleLayout *layout, View *view);
void dwindle_remove_view(DwindleLayout *layout, View *view);
void dwindle_arrange(DwindleLayout *layout, int width, int height);
void dwindle_focus(DwindleLayout *layout, View *view);
View *dwindle_focused_view(DwindleLayout *layout);
View *dwindle_first_view(DwindleLayout *layout);
void dwindle_move_focus(DwindleLayout *layout, int dx, int dy);
void dwindle_resize(DwindleLayout *layout, int dx, int dy);
bool dwindle_move_view(DwindleLayout *layout, int dx, int dy);
void dwindle_swap_views(DwindleLayout *layout, View *a, View *b);
View *dwindle_view_at(DwindleLayout *layout, int x, int y);
void dwindle_place_view(DwindleLayout *layout, View *view, View *under, int rel_x, int rel_y);
#endif
