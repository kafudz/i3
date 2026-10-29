/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * resize.c: Interactive resizing.
 *
 */
#include "all.h"

/* Minimum distance (in pixels) the resize bar keeps from the edges of the
 * output while dragging. */
#define RESIZE_EDGE_MARGIN 25

/*
 * This is an ugly data structure which we need because there is no standard
 * way of having nested functions (only available as a gcc extension at the
 * moment, clang doesn’t support it) or blocks (only available as a clang
 * extension and only on Mac OS X systems at the moment).
 *
 */
struct callback_params {
    orientation_t orientation;
    Con *output;
    xcb_window_t helpwin;
    uint32_t *new_position;
    bool *threshold_exceeded;
};

/*
 * Clamps v into [lo, hi]. The caller guarantees lo <= hi.
 *
 */
static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    return (v < lo ? lo : (v > hi ? hi : v));
}

DRAGGING_CB(resize_callback) {
    const struct callback_params *params = extra;
    Con *output = params->output;
    DLOG("new x = %d, y = %d\n", (int32_t)new_x, (int32_t)new_y);

    if (!*params->threshold_exceeded) {
        xcb_map_window(conn, params->helpwin);
        /* Warp pointer in the same way as resize_graphical_handler() would do
         * if threshold wasn't enabled, but also take into account travelled
         * distance. */
        if (params->orientation == HORIZ) {
            xcb_warp_pointer(conn, XCB_NONE, event->root, 0, 0, 0, 0,
                             *params->new_position + new_x - event->root_x,
                             new_y);
        } else {
            xcb_warp_pointer(conn, XCB_NONE, event->root, 0, 0, 0, 0,
                             new_x,
                             *params->new_position + new_y - event->root_y);
        }
        *params->threshold_exceeded = true;
        return;
    }

    const bool horiz = (params->orientation == HORIZ);

    /* Coordinates are unsigned in the callback signature, but outputs placed
     * left of or above the primary output have negative origins. Do the
     * boundary arithmetic in signed integers so that outputs which straddle
     * the origin work, too. */
    const int32_t pos = (int32_t)(horiz ? new_x : new_y);
    const int32_t start = (int32_t)(horiz ? output->rect.x : output->rect.y);
    const int32_t size = (int32_t)(horiz ? output->rect.width : output->rect.height);
    const int32_t lo = start + RESIZE_EDGE_MARGIN;
    const int32_t hi = start + size - RESIZE_EDGE_MARGIN;
    if (lo > hi) {
        /* The output is too small to leave a margin on both sides. */
        return;
    }

    /* Keep the resize bar within the screen boundaries. Clamping (instead of
     * ignoring out-of-range motion events) lets the bar reach the limit even
     * when the pointer moves quickly past it. */
    *(params->new_position) = (uint32_t)clamp_i32(pos, lo, hi);
    xcb_configure_window(conn, params->helpwin,
                         (horiz ? XCB_CONFIG_WINDOW_X : XCB_CONFIG_WINDOW_Y),
                         params->new_position);

    xcb_flush(conn);
}

bool resize_find_tiling_participants(Con **current, Con **other, direction_t direction, bool both_sides) {
    Con *first = *current;
    Con *second = NULL;
    if (first == NULL) {
        DLOG("Current container is NULL, aborting.\n");
        return false;
    }
    DLOG("Find two participants for resizing container=%p in direction=%i\n", first, direction);

    /* Go up in the tree and search for a container to resize */
    const orientation_t search_orientation = orientation_from_direction(direction);
    const bool dir_backwards = (direction == D_UP || direction == D_LEFT);
    while (first != NULL &&
           first->type != CT_WORKSPACE &&
           first->type != CT_FLOATING_CON &&
           second == NULL) {
        /* Reached the top of the tree without finding a workspace. */
        if (first->parent == NULL) {
            first = NULL;
            break;
        }

        /* get the appropriate first container with the matching
         * orientation (skip stacked/tabbed cons) */
        if ((con_orientation(first->parent) != search_orientation) ||
            (first->parent->layout == L_STACKED) ||
            (first->parent->layout == L_TABBED)) {
            first = first->parent;
            continue;
        }

        /* get the counterpart for this resizement */
        if (dir_backwards) {
            second = TAILQ_PREV(first, nodes_head, nodes);
            if (second == NULL && both_sides) {
                second = TAILQ_NEXT(first, nodes);
            }
        } else {
            second = TAILQ_NEXT(first, nodes);
            if (second == NULL && both_sides) {
                second = TAILQ_PREV(first, nodes_head, nodes);
            }
        }

        if (second == NULL) {
            DLOG("No second container in this direction found, trying to look further up in the tree...\n");
            first = first->parent;
        }
    }

    DLOG("Found participants: first=%p and second=%p.\n", first, second);
    *current = first;
    *other = second;
    if (first == NULL || second == NULL) {
        DLOG("Could not find two participants for this resize request.\n");
        return false;
    }

    return true;
}

/*
 * Calculate the minimum percent needed for the given container to be at least 1
 * pixel.
 *
 */
double percent_for_1px(Con *con) {
    const int parent_size = con_rect_size_in_orientation(con->parent);
    /* deco_rect.height is subtracted from each child in render_con_split */
    const int min_size = (con_orientation(con->parent) == HORIZ ? 1 : 1 + con->deco_rect.height);
    if (parent_size <= 0) {
        /* Not rendered (yet) or collapsed: no percentage can satisfy the
         * minimum size. Avoid dividing by zero. */
        return 1.0;
    }
    return ((double)min_size / (double)parent_size);
}

/*
 * Resize the two given containers using the given amount of pixels or
 * percentage points. One of the two needs to be 0. A positive amount means
 * growing the first container while a negative means shrinking it.
 * Returns false when the resize would result in one of the two containers
 * having less than 1 pixel of size.
 *
 */
bool resize_neighboring_cons(Con *first, Con *second, int px, int ppt) {
    /* Not `px * ppt == 0`: the multiplication can overflow (undefined
     * behavior for signed integers). */
    assert(px == 0 || ppt == 0);

    Con *parent = first->parent;
    double new_first_percent;
    double new_second_percent;
    if (ppt) {
        new_first_percent = first->percent + ((double)ppt / 100.0);
        new_second_percent = second->percent - ((double)ppt / 100.0);
    } else {
        /* Convert px change to change in percentages */
        const int parent_size = con_rect_size_in_orientation(first->parent);
        if (parent_size <= 0) {
            /* Would turn the percentages into inf/NaN, which compare false
             * against every limit below and would be stored. */
            return false;
        }
        const double pct = (double)px / (double)parent_size;
        new_first_percent = first->percent + pct;
        new_second_percent = second->percent - pct;
    }
    /* Ensure that no container will be less than 1 pixel in the resizing
     * direction. */
    if (new_first_percent < percent_for_1px(first) || new_second_percent < percent_for_1px(second)) {
        return false;
    }

    first->percent = new_first_percent;
    second->percent = new_second_percent;
    con_fix_percent(parent);
    return true;
}

/*
 * Computes the geometry of the resize bar (stored in helprect) for resizing
 * between the two given containers and returns the initial position of the bar
 * along the axis of motion (in the middle of the gap between the containers).
 *
 */
static uint32_t resize_bar_setup(Con *first, Con *second, orientation_t orientation, Rect *helprect) {
    /* The resizebar runs along the rect of the second con and follows the
     * motion of the pointer. */
    helprect->x = second->rect.x;
    helprect->y = second->rect.y;

    /* Resizes might happen between a split container and a leaf
     * container. Because gaps happen *within* a split container, we need to
     * work with (any) leaf window inside the split, so descend focused. */
    Con *ffirst = con_descend_focused(first);
    Con *fsecond = con_descend_focused(second);

    uint32_t ffirst_end;
    uint32_t fsecond_start;
    if (orientation == HORIZ) {
        helprect->width = logical_px(2);
        helprect->height = second->rect.height;
        ffirst_end = ffirst->rect.x + ffirst->rect.width;
        fsecond_start = fsecond->rect.x;
        DLOG("ffirst->rect = {.x = %u, .width = %u}\n", ffirst->rect.x, ffirst->rect.width);
        DLOG("fsecond->rect = {.x = %u, .width = %u}\n", fsecond->rect.x, fsecond->rect.width);
    } else {
        helprect->width = second->rect.width;
        helprect->height = logical_px(2);
        ffirst_end = ffirst->rect.y + ffirst->rect.height;
        fsecond_start = fsecond->rect.y;
        DLOG("ffirst->rect = {.y = %u, .height = %u}\n", ffirst->rect.y, ffirst->rect.height);
        DLOG("fsecond->rect = {.y = %u, .height = %u}\n", fsecond->rect.y, fsecond->rect.height);
    }

    const uint32_t gap = fsecond_start - ffirst_end;
    const uint32_t middle = fsecond_start - (gap / 2);
    DLOG("gap = %u, middle = %u\n", gap, middle);
    return middle;
}

void resize_graphical_handler(Con *first, Con *second, orientation_t orientation,
                              const xcb_button_press_event_t *event,
                              bool use_threshold) {
    Con *output = con_get_output(first);
    if (output == NULL) {
        ELOG("Cannot resize graphically: container %p is not on any output.\n", first);
        return;
    }
    DLOG("x = %d, width = %d\n", output->rect.x, output->rect.width);
    /* Split containers have no name. */
    DLOG("first = %p / %s\n", first, first->name ? first->name : "(unnamed)");
    DLOG("second = %p / %s\n", second, second->name ? second->name : "(unnamed)");

    x_mask_event_mask(~XCB_EVENT_MASK_ENTER_WINDOW);
    xcb_flush(conn);

    uint32_t mask = 0;
    uint32_t values[2];

    mask = XCB_CW_OVERRIDE_REDIRECT;
    values[0] = 1;

    /* Open a new window, the resizebar. Grab the pointer and move the window
     * around as the user moves the pointer. */
    xcb_window_t grabwin = create_window(conn, output->rect, XCB_COPY_FROM_PARENT, XCB_COPY_FROM_PARENT,
                                         XCB_WINDOW_CLASS_INPUT_ONLY, XCURSOR_CURSOR_POINTER, true, mask, values);

    /* Keep track of the coordinate orthogonal to motion so we can determine the
     * length of the resize afterward. */
    uint32_t new_position;

    /* Configure the resizebar and snap the pointer. */
    Rect helprect;
    const uint32_t initial_position = resize_bar_setup(first, second, orientation, &helprect);

    mask = XCB_CW_BACK_PIXEL;
    values[0] = config.client.focused.border.colorpixel;

    mask |= XCB_CW_OVERRIDE_REDIRECT;
    values[1] = 1;

    xcb_window_t helpwin = create_window(conn, helprect, XCB_COPY_FROM_PARENT, XCB_COPY_FROM_PARENT,
                                         XCB_WINDOW_CLASS_INPUT_OUTPUT, (orientation == HORIZ ? XCURSOR_CURSOR_RESIZE_HORIZONTAL : XCURSOR_CURSOR_RESIZE_VERTICAL), false, mask, values);

    if (!use_threshold) {
        xcb_map_window(conn, helpwin);
        if (orientation == HORIZ) {
            xcb_warp_pointer(conn, XCB_NONE, event->root, 0, 0, 0, 0,
                             initial_position, event->root_y);
        } else {
            xcb_warp_pointer(conn, XCB_NONE, event->root, 0, 0, 0, 0,
                             event->root_x, initial_position);
        }
    }

    xcb_circulate_window(conn, XCB_CIRCULATE_RAISE_LOWEST, helpwin);

    xcb_flush(conn);

    /* `new_position' will be updated by the `resize_callback'. */
    new_position = initial_position;

    bool threshold_exceeded = !use_threshold;

    const struct callback_params params = {orientation, output, helpwin, &new_position, &threshold_exceeded};

    /* Re-render the tree before returning to the event loop (drag_pointer()
     * runs its own event-loop) in case if there are unrendered updates. */
    tree_render();

    /* `drag_pointer' blocks until the drag is completed. */
    drag_result_t drag_result = drag_pointer(NULL, event, grabwin, 0, use_threshold, resize_callback, &params);

    xcb_destroy_window(conn, helpwin);
    xcb_destroy_window(conn, grabwin);
    xcb_flush(conn);

    /* User cancelled the drag so no action should be taken. */
    if (drag_result == DRAG_REVERT) {
        return;
    }

    /* The positions are unsigned and may wrap for negative coordinates; the
     * difference is a small signed value either way. */
    const int pixels = (int32_t)(new_position - initial_position);
    DLOG("Done, pixels = %d\n", pixels);

    /* No change; no action needed. */
    if (pixels == 0) {
        return;
    }

    /* if we got thus far, the containers must have valid percentages. */
    assert(first->percent > 0.0);
    assert(second->percent > 0.0);
    const bool result = resize_neighboring_cons(first, second, pixels, 0);
    DLOG("Graphical resize %s: first->percent = %f, second->percent = %f.\n",
         result ? "successful" : "failed", first->percent, second->percent);
}
