/* OX: OpenChannel NVM Express SSD Controller
 *
 * Copyright (C) 2016, IT University of Copenhagen. All rights reserved.
 * Written by          Ivan Luiz Picoli <ivpi@itu.dk>
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
#include <stdint.h>
#include <string.h>
#include "hw/block/ox-ctrl/include/ssd.h"

static int map_init (void)
{
    return 0;
}

static void map_exit (void)
{

}

static int map_upsert (uint64_t lba, uint64_t ppa)
{
    return 0;
}

static uint64_t map_read (uint64_t lba)
{
    return 0;
}

void gl_map_register (void) {
    appnvm()->gl_map.init_fn = map_init;
    appnvm()->gl_map.exit_fn = map_exit;
    appnvm()->gl_map.upsert_fn = map_upsert;
    appnvm()->gl_map.read_fn = map_read;
}