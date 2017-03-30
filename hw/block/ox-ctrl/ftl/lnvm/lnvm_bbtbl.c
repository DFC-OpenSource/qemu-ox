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
#include "ftl_lnvm.h"

extern struct core_struct core;

static int lnvm_contains_ppa (struct nvm_ppa_addr *list, uint32_t list_sz,
                                                        struct nvm_ppa_addr ppa)
{
    int i;

    if (!list)
        return 0;

    for (i = 0; i < list_sz; i++)
        if (ppa.ppa == list[i].ppa)
            return 1;

    return 0;
}

/* Erases the entire channel, failed erase marks the block as bad
 * bbt -> bad block table pointer to be filled up
 * bbt_sz -> pointer to integer, function will set it up
 * ch  -> channel to be checked
 */
static struct nvm_ppa_addr *lnvm_check_ch_bb (struct nvm_ppa_addr *bbt,
                                     uint16_t *bbt_sz,  struct nvm_channel *ch)
{
    int ret, i, pl, blk, lun, bb_count = 0;
    struct nvm_mmgr_io_cmd *cmd;

    log_info("    [lnvm: Checking bad blocks on channel %d...]\n",ch->ch_id);

    /* Prevents channel erasing if not in test mode (disabled in QEMU) */
    /*
    if (!core.tests_init->init) {
        log_info("[lnvm ERR: Please, run in test mode.]\n");
        return NULL;
    }
    */

    cmd = malloc(sizeof(struct nvm_mmgr_io_cmd));
    if (!cmd)
        return NULL;

    for (lun = 0; lun < ch->geometry->lun_per_ch; lun++) {
        for (blk = 0; blk < ch->geometry->blk_per_lun; blk++) {
            for (pl = 0; pl < ch->geometry->n_of_planes; pl++) {
                memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
                cmd->ppa.g.blk = blk;
                cmd->ppa.g.pl = pl;
                cmd->ppa.g.ch = ch->ch_mmgr_id;
                cmd->ppa.g.lun = lun;
                cmd->ppa.g.pg = 0;

                /* Prevents erasing reserved blocks */
                if (lnvm_contains_ppa(ch->mmgr_rsv_list, ch->mmgr_rsv,cmd->ppa))
                    continue;
                if (lnvm_contains_ppa(ch->ftl_rsv_list, ch->ftl_rsv,cmd->ppa))
                    continue;

                ret = nvm_submit_sync_io (ch, cmd, NULL, MMGR_ERASE_BLK);

                if (ret) {
                    /* Avoids adding the same block (multiple plane failure) */
                    if (lnvm_contains_ppa(bbt, bb_count, cmd->ppa))
                        continue;

                    bb_count = bb_count + ch->geometry->n_of_planes;
                    bbt = realloc(bbt, sizeof(struct nvm_ppa_addr) * bb_count);

                    /* fill up bb table for all planes */
                    for (i = ch->geometry->n_of_planes; i > 0; i--) {
                        memcpy(&bbt[bb_count - i], &cmd->ppa, sizeof(uint64_t));
                        bbt[bb_count - i].g.pl = ch->geometry->n_of_planes - i;
                    }
                    log_info("      [lnvm: bad block: lun %d, blk %d\n",
                                                                      lun, blk);
                }
            }
        }
    }
    free(cmd);
    *bbt_sz = bb_count;

    return bbt;
}

static int lnvm_io_rsv_blk (struct nvm_channel *ch, uint8_t cmdtype,
                                                   void **buf_vec, uint16_t pg)
{
    int pl, ret = -1;
    void *buf = NULL;
    struct nvm_mmgr_io_cmd *cmd = malloc(sizeof(struct nvm_mmgr_io_cmd));
    if (!cmd)
        return EMEM;

    for (pl = 0; pl < ch->geometry->n_of_planes; pl++) {
        memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
        cmd->ppa.g.blk = ch->mmgr_rsv;
        cmd->ppa.g.pl = pl;
        cmd->ppa.g.ch = ch->ch_mmgr_id;
        cmd->ppa.g.lun = 0;
        cmd->ppa.g.pg = pg;

        if (cmdtype != MMGR_ERASE_BLK)
            buf = buf_vec[pl];

        ret = nvm_submit_sync_io (ch, cmd, buf, cmdtype);
        if (ret)
            break;
    }
    free(cmd);

    return ret;
}

static int lnvm_count_bb (struct lnvm_channel *lch)
{
    int i, bb = 0;

    for (i = 0; i < lch->bbtbl->bb_sz; i++)
        if (lch->bbtbl->tbl[i] != 0x0)
            bb++;

    return bb;
}

int lnvm_flush_bbt (struct lnvm_channel *lch, struct lnvm_bbtbl *bbt)
{
    int ret, pg, i;
    struct lnvm_bbtbl nvm_bbt;
    struct nvm_channel *ch = lch->ch;
    uint8_t n_pl = ch->geometry->n_of_planes;
    uint32_t pg_sz = ch->geometry->pg_size;
    uint8_t *buf;
    uint16_t buf_sz = pg_sz + ch->geometry->sec_oob_sz
                                                    * ch->geometry->sec_per_pg;
    void *buf_vec[n_pl];

    if (bbt->bb_sz > pg_sz) {
        log_err("[lnvm ERR: Ch %d -> Maximum Bad block Table size: %d blocks\n",
                                                            ch->ch_id, pg_sz);
        return -1;
    }

    buf = calloc(buf_sz * n_pl, 1);
    if (!buf)
        return EMEM;

    for (i = 0; i < n_pl; i++)
        buf_vec[i] = buf + i * buf_sz;

    pg = 0;
    do {
        memset (buf, 0, buf_sz * n_pl);
        ret = lnvm_io_rsv_blk (ch, MMGR_READ_PG, buf_vec, pg);

        /* get info from OOB area */
        memcpy(&nvm_bbt, buf + pg_sz, sizeof(struct lnvm_bbtbl));

        if (ret || nvm_bbt.magic != FTL_LNVM_MAGIC)
            break;

        pg++;
    } while (pg < ch->geometry->pg_per_blk);

    if (ret)
        goto OUT;

    if (pg == ch->geometry->pg_per_blk) {
        if (lnvm_io_rsv_blk (ch, MMGR_ERASE_BLK, NULL, 0))
            goto OUT;
        pg = 0;
    }

    memset (buf, 0, buf_sz * n_pl);

    /* set info to OOB area */
    bbt->magic = FTL_LNVM_MAGIC;
    bbt->bb_count = lnvm_count_bb (lch);
    memcpy (&buf[pg_sz], bbt, sizeof(struct lnvm_bbtbl));

    /* set bad block table */
    memcpy (buf, bbt->tbl, bbt->bb_sz);

    ret = lnvm_io_rsv_blk (ch, MMGR_WRITE_PG, buf_vec, pg);

OUT:
    free(buf);
    return ret;
}

int lnvm_bbt_create (struct lnvm_channel *lch, struct lnvm_bbtbl *bbt)
{
    int i, rsv, l_addr, b_addr, pl_addr, n_pl;
    struct nvm_ppa_addr *bbt_tmp;
    uint16_t bb_count;
    struct nvm_channel *ch = lch->ch;

    n_pl = ch->geometry->n_of_planes;
    bbt_tmp = malloc (sizeof(struct nvm_ppa_addr));
    if (!bbt_tmp)
        return -1;

    memset (bbt->tbl, 0, bbt->bb_sz);

    /* Set FTL reserved bad blocks */
    for (rsv = 0; rsv < ch->ftl_rsv * n_pl; rsv++){
        l_addr = ch->ftl_rsv_list[rsv].g.lun * ch->geometry->blk_per_lun * n_pl;
        b_addr = ch->ftl_rsv_list[rsv].g.blk * n_pl;
        pl_addr = ch->ftl_rsv_list[rsv].g.pl;
        bbt->tbl[l_addr + b_addr + pl_addr] = 0x1;
    }

    /* Set MMGR reserved bad blocks */
    for (rsv = 0; rsv < ch->mmgr_rsv  * n_pl; rsv++){
        l_addr = ch->mmgr_rsv_list[rsv].g.lun * ch->geometry->blk_per_lun*n_pl;
        b_addr = ch->mmgr_rsv_list[rsv].g.blk * n_pl;
        pl_addr = ch->mmgr_rsv_list[rsv].g.pl;
        bbt->tbl[l_addr + b_addr + pl_addr] = 0x1;
    }

    /* Check for bad blocks in the whole channel */
    bbt_tmp = lnvm_check_ch_bb (bbt_tmp, &bb_count, ch);
    if (!bbt_tmp)
        return -1;
    lch->bbtbl->bb_count = bb_count;

    for (i = 0; i < bb_count; i++) {
        l_addr = bbt_tmp[i].g.lun * ch->geometry->blk_per_lun * n_pl;
        b_addr = bbt_tmp[i].g.blk * n_pl;
        pl_addr = bbt_tmp[i].g.pl;
        bbt->tbl[l_addr + b_addr + pl_addr] = 0x1;
    }

    return 0;
}

int lnvm_get_bbt_nvm (struct lnvm_channel *lch, struct lnvm_bbtbl *bbt)
{
    int ret, pg, i;
    struct lnvm_bbtbl nvm_bbt;
    struct nvm_channel *ch = lch->ch;
    uint8_t n_pl = ch->geometry->n_of_planes;
    uint32_t pg_sz = ch->geometry->pg_size;
    void *buf_vec[n_pl];
    void *buf;
    uint16_t buf_sz = pg_sz + ch->geometry->sec_oob_sz
                                                    * ch->geometry->sec_per_pg;

    if (bbt->bb_sz > pg_sz) {
        log_err("[lnvm ERR: Ch %d -> Maximum Bad block Table size: %d blocks\n",
                                                            ch->ch_id, pg_sz);
        return -1;
    }

    buf = calloc(buf_sz * n_pl, 1);
    if (!buf)
        return EMEM;

    for (i = 0; i < n_pl; i++)
        buf_vec[i] = buf + i * buf_sz;

    pg = 0;
    do {
        memset (buf, 0, buf_sz * n_pl);
        ret = lnvm_io_rsv_blk (ch, MMGR_READ_PG, buf_vec, pg);

        /* get info from OOB area */
        memcpy(&nvm_bbt, buf + pg_sz,sizeof(struct lnvm_bbtbl));

        if (ret || nvm_bbt.magic != FTL_LNVM_MAGIC)
            break;

        /* copy bad block table to channel */
        memcpy(bbt->tbl, buf, bbt->bb_sz);

        pg++;
    } while (pg < ch->geometry->pg_per_blk);

    if (ret)
        goto OUT;

    if (!pg) {
        ret = lnvm_io_rsv_blk (ch, MMGR_ERASE_BLK, NULL, 0);

        /* tells the caller that the block is new and must be written */
        bbt->magic = FTL_LNVM_MAGIC;
        goto OUT;
    }

    bbt->magic = 0;
    bbt->bb_count = lnvm_count_bb (lch);

OUT:
    free(buf);
    return ret;
}