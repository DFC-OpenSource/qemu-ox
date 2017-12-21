/* OX: OpenChannel NVM Express SSD Controller
 *
 * Copyright (C) 2016, IT University of Copenhagen. All rights reserved.
 * Written by          Ivan Luiz Picoli <ivpi@itu.dk>
 * LUN provisioning by Carla Villegas   <carv@itu.dk>
 *
 * Funding support provided by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#include <stdlib.h>
#include <stdio.h>
#include "appnvm.h"
#include <sys/queue.h>
#include <pthread.h>
#include <stdint.h>
#include "hw/block/ox-ctrl/include/ssd.h"

extern uint16_t app_nch;

static uint8_t *active_ch;
static uint8_t *check_gc;

static pthread_spinlock_t act_ch_spin;
static pthread_spinlock_t check_gc_spin;

static void flags_debug_off (uint16_t flag_id, uint16_t offset)
{
    log_info ("[appnvm (flags): Wrong offset. id: %d, off %d]", flag_id,
                                                                        offset);
    if (APPNVM_DEBUG)
        printf ("[appnvm (flags): Wrong offset. id: %d, off %d]\n", flag_id,
                                                                        offset);
}

static void flags_debug_id (uint16_t flag_id)
{
    log_info ("[appnvm (flags): Flag not found. id: %d]", flag_id);
    if (APPNVM_DEBUG)
        printf ("[appnvm (flags): Flag not found. id: %d]\n", flag_id);
}

static int flags_init (void)
{
    uint16_t bytes = app_nch / 8;
    if (app_nch % 8 > 0)
        bytes++;

    active_ch = calloc (1, bytes);
    if (!active_ch)
        return -1;

    check_gc = calloc (1, bytes);
    if (!check_gc)
        goto FREE;

    pthread_spin_init (&act_ch_spin, 0);
    pthread_spin_init (&check_gc_spin, 0);

    log_info("    [appnvm: Global flags started.]");
    log_info("     [appnvm: - active channels]");
    log_info("     [appnvm: - channel GC checking]");

    return 0;

FREE:
    free (active_ch);
    return -1;
}

static void flags_exit (void)
{
    free (active_ch);
    free (check_gc);
    pthread_spin_destroy (&act_ch_spin);
    pthread_spin_destroy (&check_gc_spin);
}

static void flags_set (uint16_t flag_id, uint16_t offset)
{
    if (offset > app_nch - 1) {
        flags_debug_off (flag_id, offset);
        return;
    }

    switch (flag_id) {
        case APP_FLAGS_ACT_CH:
            pthread_spin_lock (&act_ch_spin);
            active_ch[offset / 8] |= (1 << (offset % 8));
            pthread_spin_unlock (&act_ch_spin);
            break;
        case APP_FLAGS_CHECK_GC:
            pthread_spin_lock (&check_gc_spin);
            check_gc[offset / 8] |= (1 << (offset % 8));
            pthread_spin_unlock (&check_gc_spin);
            break;
        default:
            flags_debug_id (flag_id);
            return;
    }
}

static void flags_unset (uint16_t flag_id, uint16_t offset)
{
    if (offset > app_nch - 1) {
        flags_debug_off (flag_id, offset);
        return;
    }

    switch (flag_id) {
        case APP_FLAGS_ACT_CH:
            pthread_spin_lock (&act_ch_spin);
            if (active_ch[offset / 8] & (1 << (offset % 8)))
                active_ch[offset / 8] ^= (1 << (offset % 8));
            pthread_spin_unlock (&act_ch_spin);
            break;
        case APP_FLAGS_CHECK_GC:
            pthread_spin_lock (&check_gc_spin);
            if (check_gc[offset / 8] & (1 << (offset % 8)))
                check_gc[offset / 8] ^= (1 << (offset % 8));
            pthread_spin_unlock (&check_gc_spin);
            break;
        default:
            flags_debug_id (flag_id);
            return;
    }
}

static uint8_t flags_check (uint16_t flag_id, uint16_t offset)
{
    uint8_t state;

    if (offset > app_nch - 1) {
        flags_debug_off (flag_id, offset);
        return 0;
    }

    switch (flag_id) {
        case APP_FLAGS_ACT_CH:
            pthread_spin_lock (&act_ch_spin);
            state = active_ch[offset / 8] & (1 << (offset % 8));
            pthread_spin_unlock (&act_ch_spin);
            return state;
            break;
        case APP_FLAGS_CHECK_GC:
            pthread_spin_lock (&check_gc_spin);
            state = check_gc[offset / 8] & (1 << (offset % 8));
            pthread_spin_unlock (&check_gc_spin);
            return state;
            break;
        default:
            flags_debug_id (flag_id);
            return 0;
    }
}

void flags_register (void) {
    appnvm()->flags.init_fn = flags_init;
    appnvm()->flags.exit_fn = flags_exit;
    appnvm()->flags.set_fn = flags_set;
    appnvm()->flags.unset_fn = flags_unset;
    appnvm()->flags.check_fn = flags_check;
}
