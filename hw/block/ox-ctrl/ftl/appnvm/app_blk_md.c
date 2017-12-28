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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "hw/block/ox-ctrl/include/ssd.h"
#include "appnvm.h"

static int blk_md_create (struct app_channel *lch)
{
    int i;
    struct app_blk_md_entry *ent;
    struct app_blk_md *md = lch->blk_md;

    for (i = 0; i < md->entries; i++) {
        ent = ((struct app_blk_md_entry *) md->tbl) + i;
        memset (ent, 0x0, sizeof (struct app_blk_md_entry));
        ent->ppa.ppa = 0x0;
        ent->ppa.g.ch = lch->ch->ch_id;
        ent->ppa.g.lun = i / lch->ch->geometry->blk_per_lun;
        ent->ppa.g.blk = i % lch->ch->geometry->blk_per_lun;
    }

    return 0;
}

static int blk_md_load (struct app_channel *lch)
{
    int pg;
    struct app_blk_md *md = lch->blk_md;
    struct nvm_ppa_addr ppa;

    struct app_io_data *io = app_alloc_pg_io(lch);
    if (io == NULL)
        return -1;

    /* single page planes might be padded to avoid broken entries */
    uint16_t ent_per_pg = (io->pg_sz / sizeof (struct app_blk_md_entry)) *
                                                                      io->n_pl;
    uint16_t md_pgs = md->entries / ent_per_pg;
    if (md->entries % ent_per_pg > 0)
        md_pgs++;

    if (md_pgs > io->ch->geometry->pg_per_blk) {
        log_err("[appnvm ERR: Ch %d -> Maximum Block Metadata: %d bytes\n",
                       io->ch->ch_id, io->pg_sz * io->ch->geometry->pg_per_blk);
        goto ERR;
    }

    pg = app_blk_current_page (lch, io, lch->meta_blk, md_pgs);
    if (pg < 0)
        goto ERR;

    if (!pg) {
        if (app_io_rsv_blk (lch, MMGR_ERASE_BLK, NULL, lch->meta_blk, 0))
            goto ERR;

        /* tells the caller that the block is new and must be written */
        md->magic = APP_MAGIC;
        goto OUT;

    } else {

        /* load block metadata table from nvm */
        pg -= md_pgs;
        ppa.g.pg = pg;
        ppa.g.blk = lch->meta_blk;
        
        if (app_nvm_seq_transfer (io, &ppa, md->tbl, md_pgs, ent_per_pg,
                            md->entries, sizeof(struct app_blk_md_entry),
                            APP_TRANS_FROM_NVM, APP_IO_RESERVED))
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

static int blk_md_flush (struct app_channel *lch)
{
    int pg;
    struct app_blk_md *md = lch->blk_md;
    struct nvm_ppa_addr ppa;

    struct app_io_data *io = app_alloc_pg_io(lch);
    if (io == NULL)
        return -1;

    /* single page planes might be padded to avoid broken entries */
    uint16_t ent_per_pg = (io->pg_sz /
                                   sizeof (struct app_blk_md_entry)) * io->n_pl;
    uint16_t md_pgs = md->entries / ent_per_pg;
    if (md->entries % ent_per_pg > 0)
        md_pgs++;

    if (md_pgs > io->ch->geometry->pg_per_blk) {
        log_err("[appnvm ERR: Ch %d -> Maximum Block Metadata: %d bytes\n",
                       io->ch->ch_id, io->pg_sz * io->ch->geometry->pg_per_blk);
        goto ERR;
    }

    pg = app_blk_current_page (lch, io, lch->meta_blk, md_pgs);
    if (pg < 0)
        goto ERR;

    if (pg >= io->ch->geometry->pg_per_blk - md_pgs) {
        if (app_io_rsv_blk (lch, MMGR_ERASE_BLK, NULL, lch->meta_blk, 0))
            goto ERR;
        pg = 0;
    }

    md->magic = APP_MAGIC;
    memset (io->buf, 0, io->buf_sz);

    /* set info to OOB area */
    memcpy (&io->buf[io->pg_sz], md, sizeof(struct app_blk_md));

    /* flush the block metadata table to nvm */
    ppa.g.pg = pg;
    ppa.g.blk = lch->meta_blk;
    
    if (app_nvm_seq_transfer (io, &ppa, md->tbl, md_pgs, ent_per_pg,
                            md->entries, sizeof(struct app_blk_md_entry),
                            APP_TRANS_TO_NVM, APP_IO_RESERVED))
        goto ERR;

    app_free_pg_io(io);
    return 0;

ERR:
    app_free_pg_io(io);
    return -1;
}

static struct app_blk_md_entry *blk_md_get (struct app_channel *lch,
                                                                  uint16_t lun)
{
    struct app_blk_md *md = lch->blk_md;
    size_t lun_sz = md->entry_sz * lch->ch->geometry->blk_per_lun;

    if (!md->tbl)
        return NULL;

    return (struct app_blk_md_entry *) (md->tbl + (lun * lun_sz));
}

void blk_md_register (void) {
    appnvm()->md.create_fn = blk_md_create;
    appnvm()->md.flush_fn = blk_md_flush;
    appnvm()->md.load_fn = blk_md_load;
    appnvm()->md.get_fn = blk_md_get;
}