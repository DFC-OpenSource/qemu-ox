/* OX: OpenChannel NVM Express SSD Controller
 *
 * Copyright (C) 2016, IT University of Copenhagen. All rights reserved.
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 *
 * Funding support provided by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#include "hw/block/ox-ctrl/include/ssd.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "appnvm.h"

static int blk_md_create (struct app_channel *lch, struct app_blk_md *md)
{
    return 0;
}

static int blk_md_load (struct app_channel *lch, struct app_blk_md *md)
{
    return 0;
}

static int blk_md_flush (struct app_channel *lch, struct app_blk_md *md)
{
    return 0;
}

void blk_md_register (void) {
    appnvm()->md.create_fn = blk_md_create;
    appnvm()->md.flush_fn = blk_md_flush;
    appnvm()->md.load_fn = blk_md_load;
}