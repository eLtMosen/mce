/**
 * @file multitouch.c
 *
 * Mode Control Entity - Tracking for evdev based multitouch devices
 *
 * <p>
 *
 * Copyright (C) 2015 Jolla Ltd.
 *
 * <p>
 *
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 *
 * mce is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * mce is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mce.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "multitouch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#include <glib/gstdio.h>
#include <gio/gio.h>

#include "datapipe.h"
#include "evdev.h"
#include "mce.h"
#include "mce-log.h"

/* ========================================================================= *
 * TYPES & FUNCTIONS
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * TOUCH_POINT
 * ------------------------------------------------------------------------- */

/** Value to use for invalid touch point id */
#define MT_POINT_ID_INVAL -1

/** Value to use for touch point id when protocol does not provide ids */
#define MT_POINT_ID_DUMMY  0

/** Value to use for invalid touch point x & y coordinates */
#define MT_POINT_XY_INVAL -1

typedef struct mt_point_t mt_point_t;

/** Data for one touch point */
struct mt_point_t
{
  /** Touch point id from ABS_MT_TRACKING_ID event */
  int mtp_id;

  /** Touch point x-coordinate from ABS_MT_POSITION_X event */
  int mtp_x;

  /** Touch point y-coordinate from ABS_MT_POSITION_Y event */
  int mtp_y;
};

static void        mt_point_invalidate (mt_point_t *self);
static int         mt_point_distance2  (const mt_point_t *a, const mt_point_t *b);

/* ------------------------------------------------------------------------- *
 * TOUCH_STATE
 * ------------------------------------------------------------------------- */

/** Maximum number of simultaneous touch points to support */
#define MT_STATE_POINTS_MAX 16

typedef struct mt_state_t mt_state_t;

/** Tracking data for one multitouch/mouse input device */
struct mt_state_t
{
    /** Touch point constructed from MT protocol A events */
    mt_point_t mts_accum;

    /** Touch point constructed from mouse events (in SDK emulator) */
    mt_point_t mts_mouse;

    /** Array of touch points */
    mt_point_t mts_point_array[MT_STATE_POINTS_MAX];

    /** Index to currently constructed touch point
     *
     * MT protocol B uses explicit ABS_MT_SLOT events while
     * on protocol A increment on SYN_MT_REPORT / reset on
     * SYN_REPORT event is used. */
    size_t     mts_point_slot;

    /** Number of currently active touch points */
    size_t     mts_point_count;

    /** Currently tracked primary touch point */
    mt_point_t mts_point_tracked;

    /** Timer used for long press detection */
    guint      tap_to_unlock_timer_id;

    size_t     current_seq_max_fingers;

    /** Device type / protocol specific input event handler function */
    void     (*mts_event_handler_cb)(mt_state_t *, const struct input_event *);

    /** Timestamp from latest evdev input event */
    struct timeval mts_event_time;
};

static void        mt_state_reset          (mt_state_t *self);
static void        mt_state_update         (mt_state_t *self);

static void        mt_state_handle_event_a (mt_state_t *self, const struct input_event *ev);
static void        mt_state_handle_event_b (mt_state_t *self, const struct input_event *ev);

mt_state_t        *mt_state_create         (bool protocol_b);
void               mt_state_delete         (mt_state_t *self);

void               mt_state_handle_event   (mt_state_t *self, const struct input_event *ev);

bool               mt_state_touching       (const mt_state_t *self);

/* ------------------------------------------------------------------------- *
 * LONG PRESS HANDLER
 * ------------------------------------------------------------------------- */

/** Minimum press time to unlock screen [ms] */
#define MT_TOUCH_LONGPRESS_DELAY_MIN  200

extern void evin_iomon_generate_activity (struct input_event *ev, bool cooked, bool raw);

static gboolean mt_state_long_tap_cb(gpointer data)
{
    struct input_event *ev;
    mt_state_t *self;

    self = (mt_state_t *)data;

    if(!self)
        goto EXIT;

    self->tap_to_unlock_timer_id = 0;

    if(self->current_seq_max_fingers != 1)
        goto EXIT;

    ev = malloc(sizeof(struct input_event));

    mce_log(LL_CRUCIAL, "[longpress] double tap emulated from touch input");
    mce_log(LL_DEVEL, "[longpress] double tap emulated from touch input");

    ev->type  = EV_MSC;
    ev->code  = MSC_GESTURE;
    ev->value = GESTURE_DOUBLETAP;
 
    /* Do not generate activity if ts input is grabbed */
    if( !datapipe_get_gint(touch_grab_wanted_pipe) )
        evin_iomon_generate_activity(ev, true, true);

    submode_t submode = mce_get_submode_int32();

    /* If the event eater is active, don't send anything */
    if( submode & MCE_EVEATER_SUBMODE )
        goto EXIT;

    /* Gesture events count as actual non-synthetized
     * user activity. */
    evin_iomon_generate_activity(ev, false, true);

    /* But otherwise are handled in powerkey.c. */
    execute_datapipe(&keypress_pipe, &ev,
                     USE_INDATA, DONT_CACHE_INDATA);

EXIT:
    return FALSE;
}

/* ========================================================================= *
 * TOUCH_POINT
 * ========================================================================= */

/** Reset multi touch point to invalid state
 *
 * @param self  Multi touch point object
 */
static void
mt_point_invalidate(mt_point_t *self)
{
    self->mtp_id = MT_POINT_ID_INVAL;
    self->mtp_x  = MT_POINT_XY_INVAL;
    self->mtp_y  = MT_POINT_XY_INVAL;
}

/** Squared distance between two multi touch point objects
 *
 * @param a  1st multi touch point object
 * @param b  2nd multi touch point object
 *
 * @return Distance between the two points, squared.
 */
static int mt_point_distance2(const mt_point_t *a, const mt_point_t *b)
{
    int x = b->mtp_x - a->mtp_x;
    int y = b->mtp_y - a->mtp_y;
    return x*x + y*y;
}

/* ========================================================================= *
 * TOUCH_STATE
 * ========================================================================= */

/** Reset all tracked multitouch points back to invalid state
 *
 * @param self  Multitouch state object
 */
static void
mt_state_reset(mt_state_t *self)
{
    mt_point_invalidate(&self->mts_accum);

    for( size_t i = 0; i < MT_STATE_POINTS_MAX; ++i )
        mt_point_invalidate(self->mts_point_array + i);

    self->mts_point_slot = 0;

    if(self->tap_to_unlock_timer_id != 0) {
        g_source_remove(self->tap_to_unlock_timer_id);
        self->tap_to_unlock_timer_id = 0;
    }
}

/** Update touch position tracking state
 *
 * @param self  Multitouch state object
 */
static void
mt_state_update(mt_state_t *self)
{
    size_t finger_count = 0;

    /* Count fingers on screen and update position of one finger touch */
    for( size_t i = 0; i < MT_STATE_POINTS_MAX; ++i ) {
        if( self->mts_point_array[i].mtp_id == MT_POINT_ID_INVAL )
            continue;

        if( ++finger_count == 1 )
            self->mts_point_tracked = self->mts_point_array[i];
    }

    /* Treat mouse device in SDK simulation as one finger */
    if( self->mts_mouse.mtp_id != MT_POINT_ID_INVAL ) {
        if( ++finger_count == 1 )
            self->mts_point_tracked = self->mts_mouse;
    }

    /* Skip the rest if the number of fingers on screen does not change */
    if( self->mts_point_count == finger_count )
        return;

    /* When initial touch is detected, trigger a timeout for that sequence */
    if( self->mts_point_count == 0 ) {
        self->current_seq_max_fingers = finger_count;
        self->tap_to_unlock_timer_id =
            g_timeout_add(MT_TOUCH_LONGPRESS_DELAY_MIN,
                    mt_state_long_tap_cb, self);
    }

    /* Maintain maximum number of fingers seen on screen */
    if( self->current_seq_max_fingers < finger_count )
        self->current_seq_max_fingers = finger_count;

    /* When last touch is detected, delete the current timeout */
    if( finger_count == 0 && self->tap_to_unlock_timer_id != 0 ) {
        g_source_remove(self->tap_to_unlock_timer_id);
        self->tap_to_unlock_timer_id = 0;
    }

    self->mts_point_count = finger_count;
}

/** Handle multitouch protocol A event stream
 *
 * Also used for handling event streams from mouse devices
 *
 * @param self  Multitouch state object
 * @param ev    Input event
 */
static void
mt_state_handle_event_a(mt_state_t *self, const struct input_event *ev)
{
    switch( ev->type ) {
    case EV_KEY:
        switch( ev->code ) {
        case BTN_TOUCH:
            if( ev->value == 0 )
                mt_state_reset(self);
            break;

        case BTN_MOUSE:
            if( ev->value > 0 )
                self->mts_mouse.mtp_id = MT_POINT_ID_DUMMY;
            else
                self->mts_mouse.mtp_id = MT_POINT_ID_INVAL;
            break;

        default:
            break;
        }
        break;

    case EV_REL:
        switch( ev->code ) {
        case REL_X:
            self->mts_mouse.mtp_x += ev->value;
            break;
        case REL_Y:
            self->mts_mouse.mtp_y += ev->value;
            break;
        default:
            break;
        }
        break;

    case EV_ABS:
        switch( ev->code ) {
        case ABS_X:
            self->mts_mouse.mtp_x = ev->value;
            break;
        case ABS_Y:
            self->mts_mouse.mtp_y = ev->value;
            break;

        case ABS_MT_POSITION_X:
            self->mts_accum.mtp_x = ev->value;
            break;
        case ABS_MT_POSITION_Y:
            self->mts_accum.mtp_y = ev->value;
            break;
        case ABS_MT_TRACKING_ID:
            self->mts_accum.mtp_id = ev->value;
            break;
        default:
            break;
        }
        break;

    case EV_SYN:
        switch( ev->code ) {
        case SYN_MT_REPORT:
            if( self->mts_point_slot < MT_STATE_POINTS_MAX &&
                self->mts_accum.mtp_x != MT_POINT_XY_INVAL &&
                self->mts_accum.mtp_y != MT_POINT_XY_INVAL ) {
                if( self->mts_accum.mtp_id == MT_POINT_ID_INVAL )
                    self->mts_accum.mtp_id = MT_POINT_ID_DUMMY;
                self->mts_point_array[self->mts_point_slot++] = self->mts_accum;
            }
            mt_point_invalidate(&self->mts_accum);
            break;

        case SYN_REPORT:
            for( size_t i = self->mts_point_slot; i < MT_STATE_POINTS_MAX; ++i )
                mt_point_invalidate(self->mts_point_array + i);
            self->mts_point_slot = 0;
            break;

        default:
            break;
        }
        break;

    default:
        break;
    }
}

/** Handle multitouch protocol B event stream
 *
 * @param self  Multitouch state object
 * @param ev    Input event
 */
static void
mt_state_handle_event_b(mt_state_t *self, const struct input_event *ev)
{
    switch( ev->type ) {
    case EV_ABS:
        switch( ev->code ) {
        case ABS_MT_SLOT:
            self->mts_point_slot = ev->value;
            if( self->mts_point_slot >= MT_STATE_POINTS_MAX )
                self->mts_point_slot = MT_STATE_POINTS_MAX - 1;
            break;
        case ABS_MT_TRACKING_ID:
            self->mts_point_array[self->mts_point_slot].mtp_id = ev->value;
            break;
        case ABS_MT_POSITION_X:
            self->mts_point_array[self->mts_point_slot].mtp_x = ev->value;
            break;
        case ABS_MT_POSITION_Y:
            self->mts_point_array[self->mts_point_slot].mtp_y = ev->value;
            break;

        default:
            break;
        }
        break;

    default:
        break;
    }
}

/** Handle input event
 *
 * @param self  Multitouch state object
 * @param ev    Input event
 */
void
mt_state_handle_event(mt_state_t *self, const struct input_event *ev)
{
    self->mts_event_time = ev->time;

    self->mts_event_handler_cb(self, ev);

    if( ev->type == EV_SYN && ev->code == SYN_REPORT )
        mt_state_update(self);
}

/** Check if there is at least one finger on screen at the momement
 *
 * @param self  Multitouch state object
 *
 * @return true if fingers are on screen, false otherwise
 */
bool
mt_state_touching (const mt_state_t *self)
{
    return self && self->mts_point_count > 0;
}

/** Release multitouch state object
 *
 * @param self  Multitouch state object, or NULL
 */
void
mt_state_delete(mt_state_t *self)
{
    if( !self )
        goto EXIT;

    free(self);

EXIT:
    return;
}

/** Allocate multitouch state object
 *
 * @param protocol_b true if used for tracking multitouch protocol B device
 *
 * @return multitouch state object
 */
mt_state_t *
mt_state_create(bool protocol_b)
{
    mt_state_t *self = calloc(1, sizeof *self);

    if( !self )
        goto EXIT;

    self->mts_point_count = 0;

    self->tap_to_unlock_timer_id = 0;

    mt_state_reset(self);

    mt_point_invalidate(&self->mts_mouse);

    mt_point_invalidate(&self->mts_point_tracked);

    if( protocol_b )
        self->mts_event_handler_cb = mt_state_handle_event_b;
    else
        self->mts_event_handler_cb = mt_state_handle_event_a;

EXIT:
    return self;
}
