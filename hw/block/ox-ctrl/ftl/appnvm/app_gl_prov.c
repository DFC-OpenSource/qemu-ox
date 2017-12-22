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
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include "hw/block/ox-ctrl/include/ssd.h"

extern uint16_t app_nch;
static struct app_channel **ch;

static int gl_prov_init (void)
{
    int nch;

    ch = malloc (sizeof (struct app_channel *) * app_nch);
    if (!ch)
        return -1;

    nch = appnvm()->channels.get_list_fn (ch, app_nch);
    if (nch != app_nch)
        goto FREE;

    log_info("    [appnvm: Global Provisioning started.]\n");
    
    return 0;

FREE:
    free (ch);
    return -1;
}

static void gl_prov_exit (void)
{
    free (ch);
}

static struct nvm_ppa_addr *gl_prov_get_ppa_list (uint16_t pgs)
{
    int pg_left, ch_id;
    struct nvm_ppa_addr **list;

    list = calloc (sizeof (struct nvm_ppa_addr *) * pgs, 1);
    if (!list)
        return NULL;

    pg_left = pgs;
    ch_id = 0;
    while (pg_left) {
        if (appnvm()->flags.check_fn (APP_FLAGS_ACT_CH, ch[ch_id]->app_ch_id)) {
            goto FREE_LIST;
        }
    }
    
    return 0;

FREE_LIST:
    free (list);
    return NULL;
}

static void gl_prov_free_ppa_list (struct nvm_ppa_addr *list)
{
    free (list);
}

void gl_prov_register (void) {
    appnvm()->gl_prov.init_fn = gl_prov_init;
    appnvm()->gl_prov.exit_fn = gl_prov_exit;
    appnvm()->gl_prov.get_ppa_list_fn = gl_prov_get_ppa_list;
    appnvm()->gl_prov.free_ppa_list_fn = gl_prov_free_ppa_list;
}