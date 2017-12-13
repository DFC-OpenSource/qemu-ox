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
    printf ("Created.\n");
    return 0;
}

static int blk_md_load (struct app_channel *lch, struct app_blk_md *md)
{
    int ret, pg, i, pl, ent_left;
    struct app_blk_md nvm_md;
    struct nvm_channel *ch = lch->ch;
    uint8_t n_pl = ch->geometry->n_of_planes;
    uint32_t pg_sz = ch->geometry->pg_size;
    uint8_t *buf_vec[n_pl];
    uint8_t *buf;
    uint32_t meta_sz = ch->geometry->sec_oob_sz * ch->geometry->sec_per_pg;
    uint32_t buf_sz = pg_sz + meta_sz;

    /* single page planes might be padded to avoid broken entries */
    uint16_t ent_per_pg = (pg_sz / sizeof (struct app_blk_md_entry)) * n_pl;
    uint16_t md_pgs = md->entries / ent_per_pg;
    if (md->entries % ent_per_pg > 0)
        md_pgs++;

    if (md_pgs > ch->geometry->pg_per_blk) {
        log_err("[appnvm ERR: Ch %d -> Maximum Block Metadata: %d bytes\n",
                                  ch->ch_id, pg_sz * ch->geometry->pg_per_blk);
        return -1;
    }

    buf = calloc(buf_sz * n_pl, 1);
    if (!buf)
        return EMEM;

    for (i = 0; i < n_pl; i++)
        buf_vec[i] = buf + i * buf_sz;

    /* Finds the location of the newest data by checking APP_MAGIC */
    pg = 0;
    do {
        memset (buf, 0, buf_sz * n_pl);
        ret = app_io_rsv_blk (lch, MMGR_READ_PG, (void **) buf_vec,
                                                            lch->meta_blk, pg);

        /* get info from OOB area (64 bytes) in plane 0 */
        memcpy(&nvm_md, buf + pg_sz, sizeof(struct app_blk_md));

        if (ret || nvm_md.magic != APP_MAGIC)
            break;

        pg += md_pgs;
    } while (pg < ch->geometry->pg_per_blk - md_pgs);

    if (ret)
        goto OUT;

    if (!pg) {
        ret = app_io_rsv_blk (lch, MMGR_ERASE_BLK, NULL, lch->meta_blk, 0);

        /* tells the caller that the block is new and must be written */
        md->magic = APP_MAGIC;
        goto OUT;

    } else {

        /* load block metadata table from nvm */
        pg -= md_pgs;
        for (i = 0; i < md_pgs; i++) {
            ret = app_io_rsv_blk (lch, MMGR_READ_PG, (void **) buf_vec,
                                                         lch->meta_blk, pg + i);
            if (ret)
                goto OUT;

            ent_left = md->entries;

            /* Copy all planes data to the global table buffer */
            for (pl = 0; pl < n_pl; pl++) {
                if (ent_left >= ent_per_pg / n_pl) {
                    memcpy(md->tbl + (pg_sz * n_pl * i) + pl, buf_vec[pl],
                        (ent_per_pg / n_pl) * sizeof(struct app_blk_md_entry));
                    ent_left -= ent_per_pg / n_pl;
                } else {
                    memcpy(md->tbl + (pg_sz * n_pl * i) + pl, buf_vec[pl],
                                   ent_left * sizeof(struct app_blk_md_entry));
                    ent_left = 0;
                    break;
                }
            }
        }
    }

    md->magic = 0;

OUT:
    free(buf);
    return ret;
}

static int blk_md_flush (struct app_channel *lch, struct app_blk_md *md)
{
    printf ("Flushed\n");
    return 0;
}

void blk_md_register (void) {
    appnvm()->md.create_fn = blk_md_create;
    appnvm()->md.flush_fn = blk_md_flush;
    appnvm()->md.load_fn = blk_md_load;
}