/*
 * ty, a collection of GUI and command-line tools to manage Teensy devices
 *
 * Distributed under the MIT license (see LICENSE.txt or http://opensource.org/licenses/MIT)
 * Copyright (c) 2015 Niels Martignène <niels.martignene@gmail.com>
 */

#include "ty/common.h"
#include "compat.h"
#include "hs/device.h"
#include "hs/monitor.h"
#include "board_priv.h"
#include "ty/monitor.h"
#include "ty/system.h"
#include "ty/timer.h"

struct ty_monitor {
    int flags;

    hs_monitor *monitor;
    ty_timer *timer;

    bool enumerated;

    ty_list_head callbacks;
    int callback_id;

    ty_mutex refresh_mutex;
    ty_cond refresh_cond;
    int callback_ret;

    ty_list_head boards;
    ty_list_head missing_boards;

    ty_htable interfaces;

    void *udata;
};

struct ty_board_model {
    TY_BOARD_MODEL
};

struct callback {
    ty_list_head list;
    int id;

    ty_monitor_callback_func *f;
    void *udata;
};

#define DROP_BOARD_DELAY 15000

static void drop_callback(struct callback *callback)
{
    ty_list_remove(&callback->list);
    free(callback);
}

static int trigger_callbacks(ty_board *board, ty_monitor_event event)
{
    ty_list_foreach(cur, &board->monitor->callbacks) {
        struct callback *callback = ty_container_of(cur, struct callback, list);
        int r;

        r = (*callback->f)(board, event, callback->udata);
        if (r < 0)
            return r;
        if (r)
            drop_callback(callback);
    }

    return 0;
}

static int add_board(ty_monitor *monitor, ty_board_interface *iface, ty_board **rboard)
{
    ty_board *board;
    int r;

    board = calloc(1, sizeof(*board));
    if (!board) {
        r = ty_error(TY_ERROR_MEMORY, NULL);
        goto error;
    }
    board->refcount = 1;

    board->location = strdup(hs_device_get_location(iface->dev));
    if (!board->location) {
        r = ty_error(TY_ERROR_MEMORY, NULL);
        goto error;
    }

    r = ty_mutex_init(&board->interfaces_lock, TY_MUTEX_FAST);
    if (r < 0)
        goto error;

    ty_list_init(&board->interfaces);

    assert(iface->model);
    board->model = iface->model;
    board->serial = iface->serial;

    board->vid = hs_device_get_vid(iface->dev);
    board->pid = hs_device_get_pid(iface->dev);

    r = asprintf(&board->id, "%"PRIu64"-%s", board->serial, board->model->family->name);
    if (r < 0) {
        r = ty_error(TY_ERROR_MEMORY, NULL);
        goto error;
    }
    board->tag = board->id;

    board->monitor = monitor;
    ty_list_add_tail(&monitor->boards, &board->list);

    *rboard = board;
    return 0;

error:
    ty_board_unref(board);
    return r;
}

static void close_board(ty_board *board)
{
    ty_list_head ifaces;

    ty_mutex_lock(&board->interfaces_lock);

    ty_list_replace(&board->interfaces, &ifaces);
    memset(board->cap2iface, 0, sizeof(board->cap2iface));
    board->capabilities = 0;

    ty_mutex_unlock(&board->interfaces_lock);

    board->state = TY_BOARD_STATE_MISSING;
    trigger_callbacks(board, TY_MONITOR_EVENT_DISAPPEARED);

    ty_list_foreach(cur, &ifaces) {
        ty_board_interface *iface = ty_container_of(cur, ty_board_interface, list);

        if (iface->hnode.next)
            ty_htable_remove(&iface->hnode);

        ty_board_interface_unref(iface);
    }
}

static int add_missing_board(ty_board *board)
{
    ty_monitor *monitor = board->monitor;

    board->missing_since = ty_millis();
    if (board->missing.prev)
        ty_list_remove(&board->missing);
    ty_list_add_tail(&monitor->missing_boards, &board->missing);

    // There may be other boards waiting to be dropped, set timeout for the next in line
    board = ty_list_get_first(&monitor->missing_boards, ty_board, missing);

    return ty_timer_set(monitor->timer, ty_adjust_timeout(DROP_BOARD_DELAY, board->missing_since),
                        TY_TIMER_ONESHOT);
}

static void drop_board(ty_board *board)
{
    if (board->missing.prev)
        ty_list_remove(&board->missing);

    board->state = TY_BOARD_STATE_DROPPED;
    trigger_callbacks(board, TY_MONITOR_EVENT_DROPPED);

    ty_list_remove(&board->list);
}

static ty_board *find_board(ty_monitor *monitor, const char *location)
{
    ty_list_foreach(cur, &monitor->boards) {
        ty_board *board = ty_container_of(cur, ty_board, list);

        if (strcmp(board->location, location) == 0)
            return board;
    }

    return NULL;
}

static int open_new_interface(hs_device *dev, ty_board_interface **riface)
{
    ty_board_interface *iface;
    const char *serial;
    int r;

    iface = calloc(1, sizeof(*iface));
    if (!iface) {
        r = ty_error(TY_ERROR_MEMORY, NULL);
        goto error;
    }
    iface->refcount = 1;

    r = ty_mutex_init(&iface->open_lock, TY_MUTEX_FAST);
    if (r < 0)
        goto error;

    iface->dev = hs_device_ref(dev);

    serial = hs_device_get_serial_number_string(dev);
    if (serial)
        iface->serial = strtoull(serial, NULL, 10);

    r = 0;
    for (const ty_board_family **cur = ty_board_families; *cur && !r; cur++) {
        const ty_board_family *family = *cur;

        ty_error_mask(TY_ERROR_NOT_FOUND);
        r = (*family->open_interface)(iface);
        ty_error_unmask();
        if (r < 0) {
            // FIXME: propagate the errors when the initial enumeration abortion problem is fixed
            if (r == TY_ERROR_NOT_FOUND || r == TY_ERROR_ACCESS)
                r = 0;
            goto error;
        }
    }
    if (!r)
        goto error;

    *riface = iface;
    return 1;

error:
    ty_board_interface_unref(iface);
    return r;
}

static ty_board_interface *find_interface(ty_monitor *monitor, hs_device *dev)
{
    ty_htable_foreach_hash(cur, &monitor->interfaces, ty_htable_hash_ptr(dev)) {
        ty_board_interface *iface = ty_container_of(cur, ty_board_interface, hnode);

        if (iface->dev == dev)
            return iface;
    }

    return NULL;
}

static bool iface_is_compatible(ty_board_interface *iface, ty_board *board)
{
    if (ty_board_model_is_real(iface->model) && ty_board_model_is_real(board->model)
            && iface->model != board->model)
        return false;
    if (iface->serial && board->serial && iface->serial != board->serial)
        return false;

    return true;
}

static int add_interface(ty_monitor *monitor, hs_device *dev)
{
    ty_board_interface *iface = NULL;
    ty_board *board = NULL;
    ty_monitor_event event;
    int r;

    r = open_new_interface(dev, &iface);
    if (r <= 0)
        goto error;

    board = find_board(monitor, hs_device_get_location(dev));

    /* Maybe the device notifications came in the wrong order, or somehow the device
       removal notifications were dropped somewhere and we never got them, so use
       heuristics to improve board change detection. */
    if (board && !iface_is_compatible(iface, board)) {
        if (board->state == TY_BOARD_STATE_ONLINE)
            close_board(board);
        drop_board(board);

        ty_board_unref(board);
        board = NULL;
    }

    if (board) {
        if (board->vid != hs_device_get_vid(dev) || board->pid != hs_device_get_pid(dev)) {
            if (board->state == TY_BOARD_STATE_ONLINE)
                close_board(board);

            board->vid = hs_device_get_vid(dev);
            board->pid = hs_device_get_pid(dev);
        }

        if (ty_board_model_is_real(iface->model))
            board->model = iface->model;
        if (iface->serial)
            board->serial = iface->serial;

        event = TY_MONITOR_EVENT_CHANGED;
    } else {
        r = add_board(monitor, iface, &board);
        if (r < 0)
            goto error;

        event = TY_MONITOR_EVENT_ADDED;
    }

    iface->board = board;

    ty_mutex_lock(&board->interfaces_lock);

    ty_list_add_tail(&board->interfaces, &iface->list);
    ty_htable_add(&monitor->interfaces, ty_htable_hash_ptr(iface->dev), &iface->hnode);

    for (int i = 0; i < (int)TY_COUNTOF(board->cap2iface); i++) {
        if (iface->capabilities & (1 << i))
            board->cap2iface[i] = iface;
    }
    board->capabilities |= iface->capabilities;

    ty_mutex_unlock(&board->interfaces_lock);

    if (board->missing.prev)
        ty_list_remove(&board->missing);

    board->state = TY_BOARD_STATE_ONLINE;
    return trigger_callbacks(board, event);

error:
    ty_board_interface_unref(iface);
    return r;
}

static int remove_interface(ty_monitor *monitor, hs_device *dev)
{
    ty_board_interface *iface;
    ty_board *board;
    int r;

    iface = find_interface(monitor, dev);
    if (!iface)
        return 0;

    board = iface->board;

    ty_mutex_lock(&board->interfaces_lock);

    ty_htable_remove(&iface->hnode);
    ty_list_remove(&iface->list);

    memset(board->cap2iface, 0, sizeof(board->cap2iface));
    board->capabilities = 0;

    ty_list_foreach(cur, &board->interfaces) {
        iface = ty_container_of(cur, ty_board_interface, list);

        for (unsigned int i = 0; i < TY_COUNTOF(board->cap2iface); i++) {
            if (iface->capabilities & (1 << i))
                board->cap2iface[i] = iface;
        }
        board->capabilities |= iface->capabilities;
    }

    ty_mutex_unlock(&board->interfaces_lock);

    if (ty_list_is_empty(&board->interfaces)) {
        close_board(board);
        r = add_missing_board(board);
    } else {
        r = trigger_callbacks(board, TY_MONITOR_EVENT_CHANGED);
    }

    ty_board_interface_unref(iface);

    return r;
}

static int device_callback(hs_device *dev, void *udata)
{
    ty_monitor *monitor = udata;

    switch (hs_device_get_status(dev)) {
    case HS_DEVICE_STATUS_ONLINE:
        monitor->callback_ret = add_interface(monitor, dev);
        return !!monitor->callback_ret;

    case HS_DEVICE_STATUS_DISCONNECTED:
        monitor->callback_ret = remove_interface(monitor, dev);
        return !!monitor->callback_ret;
    }

    assert(false);
    return 0;
}

// FIXME: improve the sequential/parallel API
int ty_monitor_new(int flags, ty_monitor **rmonitor)
{
    assert(rmonitor);

    ty_monitor *monitor;
    int r;

    monitor = calloc(1, sizeof(*monitor));
    if (!monitor) {
        r = ty_error(TY_ERROR_MEMORY, NULL);
        goto error;
    }

    monitor->flags = flags;

    r = hs_monitor_new(NULL, 0, &monitor->monitor);
    if (r < 0) {
        r = _ty_libhs_translate_error(r);
        goto error;
    }

    r = hs_monitor_start(monitor->monitor);
    if (r < 0) {
        r = _ty_libhs_translate_error(r);
        goto error;
    }

    r = ty_timer_new(&monitor->timer);
    if (r < 0)
        goto error;

    r = ty_mutex_init(&monitor->refresh_mutex, TY_MUTEX_FAST);
    if (r < 0)
        goto error;

    r = ty_cond_init(&monitor->refresh_cond);
    if (r < 0)
        goto error;

    ty_list_init(&monitor->boards);
    ty_list_init(&monitor->missing_boards);

    r = ty_htable_init(&monitor->interfaces, 64);
    if (r < 0)
        goto error;

    ty_list_init(&monitor->callbacks);

    *rmonitor = monitor;
    return 0;

error:
    ty_monitor_free(monitor);
    return r;
}

void ty_monitor_free(ty_monitor *monitor)
{
    if (monitor) {
        ty_cond_release(&monitor->refresh_cond);
        ty_mutex_release(&monitor->refresh_mutex);

        hs_monitor_free(monitor->monitor);
        ty_timer_free(monitor->timer);

        ty_list_foreach(cur, &monitor->callbacks) {
            struct callback *callback = ty_container_of(cur, struct callback, list);
            free(callback);
        }

        ty_list_foreach(cur, &monitor->boards) {
            ty_board *board = ty_container_of(cur, ty_board, list);
            ty_board_unref(board);
        }

        ty_htable_release(&monitor->interfaces);
    }

    free(monitor);
}

void ty_monitor_set_udata(ty_monitor *monitor, void *udata)
{
    assert(monitor);
    monitor->udata = udata;
}

void *ty_monitor_get_udata(const ty_monitor *monitor)
{
    assert(monitor);
    return monitor->udata;
}

void ty_monitor_get_descriptors(const ty_monitor *monitor, ty_descriptor_set *set, int id)
{
    assert(monitor);
    assert(set);

    ty_descriptor_set_add(set, hs_monitor_get_descriptor(monitor->monitor), id);
    ty_timer_get_descriptors(monitor->timer, set, id);
}

int ty_monitor_register_callback(ty_monitor *monitor, ty_monitor_callback_func *f, void *udata)
{
    assert(monitor);
    assert(f);

    struct callback *callback = calloc(1, sizeof(*callback));
    if (!callback)
        return ty_error(TY_ERROR_MEMORY, NULL);

    callback->id = monitor->callback_id++;
    callback->f = f;
    callback->udata = udata;

    ty_list_add_tail(&monitor->callbacks, &callback->list);

    return callback->id;
}

void ty_monitor_deregister_callback(ty_monitor *monitor, int id)
{
    assert(monitor);
    assert(id >= 0);

    ty_list_foreach(cur, &monitor->callbacks) {
        struct callback *callback = ty_container_of(cur, struct callback, list);
        if (callback->id == id) {
            drop_callback(callback);
            break;
        }
    }
}

int ty_monitor_refresh(ty_monitor *monitor)
{
    assert(monitor);

    int r;

    if (ty_timer_rearm(monitor->timer)) {
        ty_list_foreach(cur, &monitor->missing_boards) {
            ty_board *board = ty_container_of(cur, ty_board, missing);
            int timeout;

            timeout = ty_adjust_timeout(DROP_BOARD_DELAY, board->missing_since);
            if (timeout) {
                r = ty_timer_set(monitor->timer, timeout, TY_TIMER_ONESHOT);
                if (r < 0)
                    return r;
                break;
            }

            drop_board(board);
            ty_board_unref(board);
        }
    }

    if (!monitor->enumerated) {
        monitor->enumerated = true;

        // FIXME: never listed devices if error on enumeration (unlink the real refresh)
        r = hs_monitor_list(monitor->monitor, device_callback, monitor);
        if (r < 0)
            return r;

        return 0;
    }

    r = hs_monitor_refresh(monitor->monitor, device_callback, monitor);
    if (r < 0) {
        /* The callback is in libty, and we need a way to get the error code without it
           being converted from a libhs error code. */
        if (monitor->callback_ret) {
            r = monitor->callback_ret;
            monitor->callback_ret = 0;
            return r;
        }

        return _ty_libhs_translate_error(r);
    }

    ty_mutex_lock(&monitor->refresh_mutex);
    ty_cond_broadcast(&monitor->refresh_cond);
    ty_mutex_unlock(&monitor->refresh_mutex);

    return 0;
}

int ty_monitor_wait(ty_monitor *monitor, ty_monitor_wait_func *f, void *udata, int timeout)
{
    assert(monitor);
    assert(f || !(monitor->flags & TY_MONITOR_PARALLEL_WAIT));

    ty_descriptor_set set = {0};
    uint64_t start;
    int r;

    start = ty_millis();
    if (monitor->flags & TY_MONITOR_PARALLEL_WAIT) {
        ty_mutex_lock(&monitor->refresh_mutex);
        while (!(r = (*f)(monitor, udata))) {
            r = ty_cond_wait(&monitor->refresh_cond, &monitor->refresh_mutex, ty_adjust_timeout(timeout, start));
            if (!r)
                break;
        }
        ty_mutex_unlock(&monitor->refresh_mutex);

        return r;
    } else {
        ty_monitor_get_descriptors(monitor, &set, 1);

        do {
            r = ty_monitor_refresh(monitor);
            if (r < 0)
                return (int)r;

            if (f) {
                r = (*f)(monitor, udata);
                if (r)
                    return r;
            }

            r = ty_poll(&set, ty_adjust_timeout(timeout, start));
        } while (r > 0);
        return r;
    }
}

int ty_monitor_list(ty_monitor *monitor, ty_monitor_callback_func *f, void *udata)
{
    assert(monitor);
    assert(f);

    ty_list_foreach(cur, &monitor->boards) {
        ty_board *board = ty_container_of(cur, ty_board, list);
        int r;

        if (board->state == TY_BOARD_STATE_ONLINE) {
            r = (*f)(board, TY_MONITOR_EVENT_ADDED, udata);
            if (r)
                return r;
        }
    }

    return 0;
}
