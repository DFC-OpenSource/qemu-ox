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

static int ch_map_create (struct app_channel *lch)
{
    int i;
    struct app_map_entry *ent;
    struct app_map_md *md = lch->map_md;

    for (i = 0; i < md->entries; i++) {
        ent = ((struct app_map_entry *) md->tbl) + i;
        memset (ent, 0x0, sizeof (struct app_map_entry));
        ent->lba = i;
    }

    return 0;
}

static int ch_map_load (struct app_channel *lch)
{
    int pg;
    struct app_map_md *md = lch->map_md;

    struct app_io_data *io = app_alloc_pg_io(lch);
    if (io == NULL)
        return -1;

    /* single page planes might be padded to avoid broken entries */
    uint16_t ent_per_pg = (io->pg_sz / sizeof(struct app_map_entry)) * io->n_pl;
    uint16_t md_pgs = md->entries / ent_per_pg;
    if (md->entries % ent_per_pg > 0)
        md_pgs++;

    if (md_pgs > io->ch->geometry->pg_per_blk) {
        log_err("[appnvm ERR: Ch %d -> Maximum Mapping Metadata: %d bytes\n",
                       io->ch->ch_id, io->pg_sz * io->ch->geometry->pg_per_blk);
        goto ERR;
    }

    pg = app_blk_current_page (lch, io, lch->map_blk, md_pgs);
    if (pg < 0)
        goto ERR;

    if (!pg) {
        if (app_io_rsv_blk (lch, MMGR_ERASE_BLK, NULL, lch->map_blk, 0))
            goto ERR;

        /* tells the caller that the block is new and must be written */
        md->magic = APP_MAGIC;
        goto OUT;

    } else {

        /* load mapping metadata table from nvm */
        pg -= md_pgs;
        if (app_meta_transfer (io, md->tbl, md_pgs, pg, ent_per_pg, md->entries,
            sizeof(struct app_map_entry), lch->map_blk, APP_TRANS_FROM_NVM))
                goto ERR;
    }

    md->magic = 0;

OUT:
    app_free_pg_io(io);
    return 0;

ERR:
    app_free_pg_io(io);
    return -1;
}

static int ch_map_flush (struct app_channel *lch)
{
    int pg;
    struct app_map_md *md = lch->map_md;

    struct app_io_data *io = app_alloc_pg_io(lch);
    if (io == NULL)
        return -1;

    /* single page planes might be padded to avoid broken entries */
    uint16_t ent_per_pg = (io->pg_sz / sizeof(struct app_map_entry)) * io->n_pl;
    uint16_t md_pgs = md->entries / ent_per_pg;
    if (md->entries % ent_per_pg > 0)
        md_pgs++;

    if (md_pgs > io->ch->geometry->pg_per_blk) {
        log_err("[appnvm ERR: Ch %d -> Maximum Mapping Metadata: %d bytes\n",
                       io->ch->ch_id, io->pg_sz * io->ch->geometry->pg_per_blk);
        goto ERR;
    }

    pg = app_blk_current_page (lch, io, lch->map_blk, md_pgs);
    if (pg < 0)
        goto ERR;

    if (pg >= io->ch->geometry->pg_per_blk - md_pgs) {
        if (app_io_rsv_blk (lch, MMGR_ERASE_BLK, NULL, lch->map_blk, 0))
            goto ERR;
        pg = 0;
    }

    md->magic = APP_MAGIC;
    memset (io->buf, 0, io->buf_sz);

    /* set info to OOB area */
    memcpy (&io->buf[io->pg_sz], md, sizeof(struct app_map_md));

    /* flush the mapping metadata table to nvm */
    if (app_meta_transfer (io, md->tbl, md_pgs, pg, ent_per_pg, md->entries,
                sizeof(struct app_map_entry), lch->map_blk, APP_TRANS_TO_NVM))
        goto ERR;

    app_free_pg_io(io);
    return 0;

ERR:
    app_free_pg_io(io);
    return -1;
}

static struct app_map_entry *ch_map_get (struct app_channel *lch, uint32_t off)
{
    return ((struct app_map_entry *) lch->map_md->tbl) + off;
}

void ch_map_register (void) {
    appnvm()->ch_map.create_fn = ch_map_create;
    appnvm()->ch_map.load_fn = ch_map_load;
    appnvm()->ch_map.flush_fn = ch_map_flush;
    appnvm()->ch_map.get_fn = ch_map_get;
}