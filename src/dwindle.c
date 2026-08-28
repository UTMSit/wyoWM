#include "layout.h"
#include <stdlib.h>
#include <math.h>
#include <float.h>

static DwindleNode *node_create(void) {
    DwindleNode *node = calloc(1, sizeof(DwindleNode));
    if (!node) return NULL;

    node->split_ratio = 0.5;

    return node;
}

static void node_destroy_recursive(DwindleNode *node) {
    if (!node) return;

    node_destroy_recursive(node->children[0]);
    node_destroy_recursive(node->children[1]);

    free(node);
}

static DwindleNode *find_leaf(DwindleNode *node, View *view) {
    if (!node) return NULL;
    if (node->view == view) return node;

    DwindleNode *found = find_leaf(node->children[0], view);
    if (found) return found;

    return find_leaf(node->children[1], view);
}

static DwindleNode *first_leaf(DwindleNode *node) {
    if (!node) return NULL;

    while (!node->view) {
        if (node->children[0]) {
            node = node->children[0];
        } else if (node->children[1]) {
            node = node->children[1];
        } else {
            return NULL;
        }
    }

    return node;
}

static void replace_child(DwindleNode *parent, DwindleNode *old, DwindleNode *new_child) {
    if (!parent) return;

    if (parent->children[0] == old) {
        parent->children[0] = new_child;
    } else if (parent->children[1] == old) {
        parent->children[1] = new_child;
    }

    if (new_child) {
        new_child->parent = parent;
    }
}

static double clamp_ratio(double ratio) {
    if (ratio < 0.1) return 0.1;
    if (ratio > 0.9) return 0.9;
    return ratio;
}

static void arrange_node(DwindleNode *node, int gaps_in) {
    if (!node) return;

    if (node->width <= 0 || node->height <= 0) return;

    if (node->view) {
        view_set_geometry(node->view, node->x, node->y, node->width, node->height);
        return;
    }

    DwindleNode *a = node->children[0];
    DwindleNode *b = node->children[1];

    if (!a && !b) return;

    if (!a) {
        b->x = node->x;
        b->y = node->y;
        b->width = node->width;
        b->height = node->height;
        arrange_node(b, gaps_in);
        return;
    }

    if (!b) {
        a->x = node->x;
        a->y = node->y;
        a->width = node->width;
        a->height = node->height;
        arrange_node(a, gaps_in);
        return;
    }

    double ratio = clamp_ratio(node->split_ratio);
    int gap = gaps_in;

    if (node->split == SPLIT_VERTICAL) {
        if (node->width - gap <= 0) {
            gap = 0;
        }

        int available = node->width - gap;
        int width_a = (int)((double)available * ratio);
        int width_b = available - width_a;

        if (width_a < 1) {
            width_a = 1;
            width_b = available - width_a;
        }

        if (width_b < 1) {
            width_b = 1;
            width_a = available - width_b;
        }

        a->x = node->x;
        a->y = node->y;
        a->width = width_a;
        a->height = node->height;

        b->x = node->x + width_a + gap;
        b->y = node->y;
        b->width = width_b;
        b->height = node->height;
    } else {
        if (node->height - gap <= 0) {
            gap = 0;
        }

        int available = node->height - gap;
        int height_a = (int)((double)available * ratio);
        int height_b = available - height_a;

        if (height_a < 1) {
            height_a = 1;
            height_b = available - height_a;
        }

        if (height_b < 1) {
            height_b = 1;
            height_a = available - height_b;
        }

        a->x = node->x;
        a->y = node->y;
        a->width = node->width;
        a->height = height_a;

        b->x = node->x;
        b->y = node->y + height_a + gap;
        b->width = node->width;
        b->height = height_b;
    }

    arrange_node(a, gaps_in);
    arrange_node(b, gaps_in);
}

static int interval_gap(int a0, int a1, int b0, int b1) {
    if (a1 <= b0) return b0 - a1;
    if (b1 <= a0) return a0 - b1;
    return 0;
}

static void search_direction_edge(
    DwindleNode *node,
    DwindleNode *current,
    int dx,
    int dy,
    DwindleNode **best,
    double *best_score
) {
    if (!node || !current) return;

    if (node->view) {
        if (node != current &&
            node->width > 0 &&
            node->height > 0 &&
            current->width > 0 &&
            current->height > 0) {
            int cur_l = current->x;
            int cur_t = current->y;
            int cur_r = current->x + current->width;
            int cur_b = current->y + current->height;

            int l = node->x;
            int t = node->y;
            int r = node->x + node->width;
            int b = node->y + node->height;

            bool valid = false;
            int primary = 0;
            int cross = 0;

            if (dx < 0) {
                if (r <= cur_l) {
                    valid = true;
                    primary = cur_l - r;
                    cross = interval_gap(t, b, cur_t, cur_b);
                }
            } else if (dx > 0) {
                if (l >= cur_r) {
                    valid = true;
                    primary = l - cur_r;
                    cross = interval_gap(t, b, cur_t, cur_b);
                }
            } else if (dy < 0) {
                if (b <= cur_t) {
                    valid = true;
                    primary = cur_t - b;
                    cross = interval_gap(l, r, cur_l, cur_r);
                }
            } else if (dy > 0) {
                if (t >= cur_b) {
                    valid = true;
                    primary = t - cur_b;
                    cross = interval_gap(l, r, cur_l, cur_r);
                }
            }

            if (valid) {
                double score = (double)primary + 4.0 * (double)cross;

                if (score < *best_score) {
                    *best_score = score;
                    *best = node;
                }
            }
        }

        return;
    }

    search_direction_edge(node->children[0], current, dx, dy, best, best_score);
    search_direction_edge(node->children[1], current, dx, dy, best, best_score);
}

static void search_direction_center(
    DwindleNode *node,
    DwindleNode *current,
    int dx,
    int dy,
    DwindleNode **best,
    double *best_score
) {
    if (!node || !current) return;

    if (node->view) {
        if (node != current &&
            node->width > 0 &&
            node->height > 0 &&
            current->width > 0 &&
            current->height > 0) {
            int cur_cx = current->x + current->width / 2;
            int cur_cy = current->y + current->height / 2;

            int cand_cx = node->x + node->width / 2;
            int cand_cy = node->y + node->height / 2;

            bool valid = true;

            if (dx < 0) {
                valid = cand_cx < cur_cx;
            } else if (dx > 0) {
                valid = cand_cx > cur_cx;
            }

            if (dy < 0) {
                valid = valid && cand_cy < cur_cy;
            } else if (dy > 0) {
                valid = valid && cand_cy > cur_cy;
            }

            if (valid) {
                double score;

                if (dx != 0) {
                    score = fabs((double)cand_cx - (double)cur_cx);
                    score += 4.0 * fabs((double)cand_cy - (double)cur_cy);
                } else {
                    score = fabs((double)cand_cy - (double)cur_cy);
                    score += 4.0 * fabs((double)cand_cx - (double)cur_cx);
                }

                if (score < *best_score) {
                    *best_score = score;
                    *best = node;
                }
            }
        }

        return;
    }

    search_direction_center(node->children[0], current, dx, dy, best, best_score);
    search_direction_center(node->children[1], current, dx, dy, best, best_score);
}

static int child_index(DwindleNode *parent, DwindleNode *child) {
    if (parent->children[0] == child) return 0;
    if (parent->children[1] == child) return 1;
    return -1;
}

void dwindle_init(DwindleLayout *layout) {
    layout->root = NULL;
    layout->focused = NULL;
    layout->width = 0;
    layout->height = 0;
    layout->master_count = 1;
    layout->master_factor = 0.55;
    layout->gaps_in = 0;
    layout->gaps_out = 0;
    layout->origin_x = 0;
    layout->origin_y = 0;
}

void dwindle_destroy(DwindleLayout *layout) {
    node_destroy_recursive(layout->root);
    layout->root = NULL;
    layout->focused = NULL;
    layout->width = 0;
    layout->height = 0;
}

void dwindle_set_gaps(DwindleLayout *layout, int gaps_in, int gaps_out) {
    if (!layout) return;

    if (gaps_in < 0) gaps_in = 0;
    if (gaps_out < 0) gaps_out = 0;

    layout->gaps_in = gaps_in;
    layout->gaps_out = gaps_out;
}

void dwindle_add_view(DwindleLayout *layout, View *view) {
    if (!layout || !view) return;
    if (find_leaf(layout->root, view)) return;

    DwindleNode *leaf = node_create();
    if (!leaf) return;

    leaf->view = view;

    if (!layout->root) {
        layout->root = leaf;
        layout->focused = leaf;
        return;
    }

    DwindleNode *target = layout->focused;
    if (!target || !target->view) {
        target = first_leaf(layout->root);
    }

    if (!target) {
        layout->root = leaf;
        layout->focused = leaf;
        return;
    }

    DwindleNode *parent = node_create();
    if (!parent) {
        free(leaf);
        return;
    }

    parent->parent = target->parent;
    parent->x = target->x;
    parent->y = target->y;
    parent->width = target->width;
    parent->height = target->height;

    if (target->width > target->height) {
        parent->split = SPLIT_VERTICAL;
    } else {
        parent->split = SPLIT_HORIZONTAL;
    }

    parent->children[0] = target;
    parent->children[1] = leaf;

    target->parent = parent;
    leaf->parent = parent;

    if (target == layout->root) {
        layout->root = parent;
    } else {
        replace_child(parent->parent, target, parent);
    }

    layout->focused = leaf;
}

void dwindle_remove_view(DwindleLayout *layout, View *view) {
    if (!layout || !view) return;

    DwindleNode *leaf = find_leaf(layout->root, view);
    if (!leaf) return;

    if (leaf == layout->root) {
        free(leaf);
        layout->root = NULL;
        layout->focused = NULL;
        return;
    }

    DwindleNode *parent = leaf->parent;
    DwindleNode *grand = parent->parent;
    DwindleNode *sibling = NULL;

    if (parent->children[0] == leaf) {
        sibling = parent->children[1];
    } else {
        sibling = parent->children[0];
    }

    if (!sibling) {
        if (!grand) {
            layout->root = NULL;
        } else {
            replace_child(grand, parent, NULL);
        }

        if (layout->focused == leaf) {
            layout->focused = NULL;
        }

        free(parent);
        free(leaf);
        return;
    }

    sibling->parent = grand;

    if (!grand) {
        layout->root = sibling;
    } else {
        replace_child(grand, parent, sibling);
    }

    if (layout->focused == leaf) {
        layout->focused = first_leaf(sibling);
    }

    free(parent);
    free(leaf);
}

void dwindle_arrange(DwindleLayout *layout, int width, int height) {
    if (!layout || !layout->root) return;
    if (width <= 0 || height <= 0) return;

    layout->width = width;
    layout->height = height;

    int out = layout->gaps_out;

    if (width - out * 2 <= 0 || height - out * 2 <= 0) {
        out = 0;
    }

    layout->root->x = layout->origin_x + out;
    layout->root->y = layout->origin_y + out;
    layout->root->width = width - out * 2;
    layout->root->height = height - out * 2;

    arrange_node(layout->root, layout->gaps_in);
}

void dwindle_focus(DwindleLayout *layout, View *view) {
    if (!layout || !view) return;

    DwindleNode *leaf = find_leaf(layout->root, view);
    if (leaf) {
        layout->focused = leaf;
    }
}

View *dwindle_focused_view(DwindleLayout *layout) {
    if (!layout || !layout->focused) return NULL;
    return layout->focused->view;
}

View *dwindle_first_view(DwindleLayout *layout) {
    if (!layout) return NULL;

    DwindleNode *leaf = first_leaf(layout->root);
    return leaf ? leaf->view : NULL;
}

static DwindleNode *node_at(DwindleNode *node, int x, int y) {
    if (!node) return NULL;

    if (node->view) {
        if (x >= node->x &&
            x < node->x + node->width &&
            y >= node->y &&
            y < node->y + node->height) {
            return node;
        }

        return NULL;
    }

    DwindleNode *found = node_at(node->children[0], x, y);
    if (found) return found;

    return node_at(node->children[1], x, y);
}

View *dwindle_view_at(DwindleLayout *layout, int x, int y) {
    if (!layout) return NULL;

    DwindleNode *node = node_at(layout->root, x, y);
    return node ? node->view : NULL;
}

void dwindle_move_focus(DwindleLayout *layout, int dx, int dy) {
    if (!layout || !layout->root) return;

    if (!layout->focused || !layout->focused->view) {
        layout->focused = first_leaf(layout->root);
        return;
    }

    DwindleNode *best = NULL;
    double best_score = DBL_MAX;

    search_direction_edge(layout->root, layout->focused, dx, dy, &best, &best_score);

    if (!best) {
        best_score = DBL_MAX;
        search_direction_center(layout->root, layout->focused, dx, dy, &best, &best_score);
    }

    if (best) {
        layout->focused = best;
    }
}

void dwindle_resize(DwindleLayout *layout, int dx, int dy) {
    if (!layout || !layout->root) return;

    if (!layout->focused || !layout->focused->view) {
        layout->focused = first_leaf(layout->root);
        if (!layout->focused) return;
    }

    DwindleNode *node = layout->focused;

    while (node && node->parent) {
        DwindleNode *parent = node->parent;
        int idx = child_index(parent, node);

        if (idx < 0) break;

        if (parent->split == SPLIT_VERTICAL && dx != 0 && parent->width > 0) {
            double delta = (double)dx / (double)parent->width;
            parent->split_ratio += delta;
            parent->split_ratio = clamp_ratio(parent->split_ratio);
        }

        if (parent->split == SPLIT_HORIZONTAL && dy != 0 && parent->height > 0) {
            double delta = (double)dy / (double)parent->height;
            parent->split_ratio += delta;
            parent->split_ratio = clamp_ratio(parent->split_ratio);
        }

        node = parent;
    }

    if (layout->width > 0 && layout->height > 0) {
        dwindle_arrange(layout, layout->width, layout->height);
    }
}

bool dwindle_move_view(DwindleLayout *layout, int dx, int dy) {
    if (!layout || !layout->root) return false;

    if (!layout->focused || !layout->focused->view) {
        layout->focused = first_leaf(layout->root);
        if (!layout->focused || !layout->focused->view) return false;
    }

    DwindleNode *best = NULL;
    double best_score = DBL_MAX;

    search_direction_edge(layout->root, layout->focused, dx, dy, &best, &best_score);

    if (!best) {
        best_score = DBL_MAX;
        search_direction_center(layout->root, layout->focused, dx, dy, &best, &best_score);
    }

    if (!best || best == layout->focused || !best->view) {
        return false;
    }

    View *moved = layout->focused->view;
    View *other = best->view;

    layout->focused->view = other;
    best->view = moved;

    DwindleNode *moved_node = find_leaf(layout->root, moved);
    layout->focused = moved_node ? moved_node : best;

    if (layout->width > 0 && layout->height > 0) {
        dwindle_arrange(layout, layout->width, layout->height);
    }

    return true;
}

void dwindle_swap_views(DwindleLayout *layout, View *a, View *b) {
    if (!layout || !a || !b || a == b) return;

    DwindleNode *leaf_a = find_leaf(layout->root, a);
    DwindleNode *leaf_b = find_leaf(layout->root, b);

    if (!leaf_a || !leaf_b) return;

    leaf_a->view = b;
    leaf_b->view = a;

    if (layout->focused == leaf_a) {
        layout->focused = leaf_b;
    } else if (layout->focused == leaf_b) {
        layout->focused = leaf_a;
    }

    if (layout->width > 0 && layout->height > 0) {
        dwindle_arrange(layout, layout->width, layout->height);
    }
}

void dwindle_place_view(DwindleLayout *layout, View *view, View *under, int rel_x, int rel_y) {
    if (!layout || !view || !under) return;

    dwindle_focus(layout, under);
    dwindle_add_view(layout, view);

    DwindleNode *leaf = find_leaf(layout->root, view);
    if (!leaf || !leaf->parent) return;

    DwindleNode *parent = leaf->parent;

    bool need_swap = false;
    if (parent->split == SPLIT_VERTICAL) {
        need_swap = (rel_x < parent->x + parent->width / 2);
    } else if (parent->split == SPLIT_HORIZONTAL) {
        need_swap = (rel_y < parent->y + parent->height / 2);
    }

    if (!need_swap) return;

    DwindleNode *under_leaf = find_leaf(layout->root, under);
    if (!under_leaf || under_leaf->parent != parent) return;

    if (parent->children[0] == under_leaf && parent->children[1] == leaf) {
        parent->children[0] = leaf;
        parent->children[1] = under_leaf;
    }
}
