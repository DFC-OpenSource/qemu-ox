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

#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/queue.h>
#include "appnvm.h"

struct app_global __appnvm;

LIST_HEAD(app_ch, app_channel) app_ch_head = LIST_HEAD_INITIALIZER(app_ch_head);

static int app_submit_io (struct nvm_io_cmd *);

struct app_global *appnvm (void) {
    return &__appnvm;
}

int app_io_rsv_blk (struct app_channel *lch, uint8_t cmdtype,
                                     void **buf_vec, uint16_t blk, uint16_t pg)
{
    int pl, ret = -1;
    void *buf = NULL;
    struct nvm_channel *ch = lch->ch;
    struct nvm_mmgr_io_cmd *cmd = malloc(sizeof(struct nvm_mmgr_io_cmd));
    if (!cmd)
        return EMEM;

    for (pl = 0; pl < ch->geometry->n_of_planes; pl++) {
        memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
        cmd->ppa.g.blk = blk;
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

static struct app_channel *app_get_ch_instance(uint16_t ch_id)
{
    struct app_channel *lch;
    LIST_FOREACH(lch, &app_ch_head, entry){
        if(lch->ch->ch_mmgr_id == ch_id)
            return lch;
    }

    return NULL;
}

static void app_callback_io (struct nvm_mmgr_io_cmd *cmd)
{

}

static int app_submit_io (struct nvm_io_cmd *cmd)
{
    cmd->status.status = NVM_IO_SUCCESS;
    cmd->status.nvme_status = NVME_SUCCESS;
    nvm_complete_ftl(cmd);

    return 0;
}

static int app_reserve_blks (struct app_channel *lch)
{
    int trsv, n, pl, n_pl;
    struct nvm_ppa_addr *ppa;
    struct nvm_channel *ch = lch->ch;

    n_pl = ch->geometry->n_of_planes;

    ch->ftl_rsv = APP_RSV_BLK_COUNT;
    trsv = ch->ftl_rsv * n_pl;

    /* ftl_rsv_list is first allocated by the MMGR that also frees the memory */
    ch->ftl_rsv_list = realloc (ch->ftl_rsv_list,
                                           trsv * sizeof(struct nvm_ppa_addr));
    if (!ch->ftl_rsv_list)
        return EMEM;

    memset (ch->ftl_rsv_list, 0, trsv * sizeof(struct nvm_ppa_addr));

    for (n = 0; n < ch->ftl_rsv; n++) {
        for (pl = 0; pl < n_pl; pl++) {
            ppa = &ch->ftl_rsv_list[n_pl * n + pl];
            ppa->g.ch = ch->ch_mmgr_id;
            ppa->g.lun = 0;
            ppa->g.blk = n + ch->mmgr_rsv;
            ppa->g.pl = pl;
        }
    }

    /* Set reserved blocks */
    lch->bbt_blk  = ch->mmgr_rsv + APP_RSV_BBT_OFF;
    lch->meta_blk = ch->mmgr_rsv + APP_RSV_META_OFF;
    lch->l2p_blk  = ch->mmgr_rsv + APP_RSV_L2P_OFF;

    return 0;
}

static int app_init_bbt (struct app_channel *lch)
{
    int ret = -1;
    uint32_t tblks;
    struct app_bbtbl *bbt;
    struct nvm_channel *ch = lch->ch;
    int n_pl = ch->geometry->n_of_planes;

    tblks = ch->geometry->blk_per_lun * ch->geometry->lun_per_ch * n_pl;

    lch->bbtbl = malloc (sizeof(struct app_bbtbl));
    if (!lch->bbtbl)
        return -1;

    bbt = lch->bbtbl;
    bbt->tbl = malloc (sizeof(uint8_t) * tblks);
    if (!bbt->tbl)
        goto FREE_BBTBL;

    memset (bbt->tbl, 0, tblks);
    bbt->magic = 0;
    bbt->bb_sz = tblks;

    ret = appnvm()->bbt.load_fn (lch, bbt);
    if (ret) goto ERR;

    /* create and flush bad block table if it does not exist */
    /* this procedure will erase the entire device (only in test mode) */
    if (bbt->magic == APP_MAGIC) {
        ret = appnvm()->bbt.create_fn (lch, bbt, APP_BBT_EMERGENCY);
        if (ret) goto ERR;
        ret = appnvm()->bbt.flush_fn (lch, bbt);
        if (ret) goto ERR;
    }

    return 0;

ERR:
    free(lch->bbtbl->tbl);
FREE_BBTBL:
    free(lch->bbtbl);
    return -1;
}

static int app_init_blk_md (struct app_channel *lch)
{
    int ret = -1;
    uint32_t tblks;
    struct app_blk_md *md;
    struct nvm_channel *ch = lch->ch;
    int n_pl = ch->geometry->n_of_planes;

    tblks = ch->geometry->blk_per_lun * ch->geometry->lun_per_ch * n_pl;

    lch->blk_md = malloc (sizeof(struct app_blk_md));
    if (!lch->blk_md)
        return -1;

    md = lch->blk_md;
    md->tbl = malloc (sizeof(struct app_blk_md_entry) * tblks);
    if (!md->tbl)
        goto FREE_MD;

    memset (md->tbl, 0, tblks);
    md->magic = 0;
    md->entries = tblks;

    ret = appnvm()->md.load_fn (lch, md);
    if (ret) goto ERR;

    /* create and flush block metadata table if it does not exist */
    if (md->magic == APP_MAGIC) {
        ret = appnvm()->md.create_fn (lch, md);
        if (ret) goto ERR;
        ret = appnvm()->md.flush_fn (lch, md);
        if (ret) goto ERR;
    }

    return 0;

ERR:
    free(lch->blk_md->tbl);
FREE_MD:
    free(lch->blk_md);
    return -1;
}

static int app_init_channel (struct nvm_channel *ch)
{
    struct app_channel *lch;

    lch = malloc (sizeof(struct app_channel));
    if (!lch)
        return EMEM;

    lch->ch = ch;

    if (app_reserve_blks (lch))
        goto FREE_LCH;

    if (app_init_bbt (lch))
        goto FREE_LCH;

    if (app_init_blk_md (lch))
        goto FREE_LCH;

    LIST_INSERT_HEAD(&app_ch_head, lch, entry);
    log_info("    [appnvm: channel %d started with %d bad blocks.]\n",ch->ch_id,
                                                          lch->bbtbl->bb_count);
    return 0;

FREE_LCH:
    free(lch);
    log_err("[appnvm ERR: Ch %d -> Not possible to read/create bad block "
                                                        "table.]\n", ch->ch_id);
    return EMEM;
}

static int app_ftl_get_bbtbl (struct nvm_ppa_addr *ppa, uint8_t *bbtbl,
                                                                    uint32_t nb)
{
    struct app_channel *lch = app_get_ch_instance(ppa->g.ch);
    struct nvm_channel *ch = lch->ch;
    int l_addr = ppa->g.lun * ch->geometry->blk_per_lun *
                                                     ch->geometry->n_of_planes;

    if (nvm_memcheck(bbtbl) || nvm_memcheck(ch) ||
                                        nb != ch->geometry->blk_per_lun *
                                        (ch->geometry->n_of_planes & 0xffff))
        return -1;

    memcpy(bbtbl, &lch->bbtbl->tbl[l_addr], nb);

    return 0;
}

static int app_ftl_set_bbtbl (struct nvm_ppa_addr *ppa, uint8_t value)
{
    int l_addr, n_pl, flush, ret;
    struct app_channel *lch = app_get_ch_instance(ppa->g.ch);

    n_pl = lch->ch->geometry->n_of_planes;

    if ((ppa->g.blk * n_pl + ppa->g.pl) >
                                   (lch->ch->geometry->blk_per_lun * n_pl - 1))
        return -1;

    l_addr = ppa->g.lun * lch->ch->geometry->blk_per_lun * n_pl;

    /* flush the table if the value changes */
    flush = (lch->bbtbl->tbl[l_addr+(ppa->g.blk * n_pl + ppa->g.pl)] == value)
                                                                        ? 0 : 1;
    lch->bbtbl->tbl[l_addr + (ppa->g.blk * n_pl + ppa->g.pl)] = value;

    if (flush) {
        ret = appnvm()->bbt.flush_fn(lch, lch->bbtbl);
        if (ret)
            log_info("[ftl WARNING: Error flushing bad block table to NVM!]");
    }

    return 0;
}

static void app_exit (struct nvm_ftl *ftl)
{
    struct app_channel *lch;

    LIST_FOREACH(lch, &app_ch_head, entry){
        free(lch->bbtbl->tbl);
        free(lch->bbtbl);
    }
    while (!LIST_EMPTY(&app_ch_head)) {
        lch = LIST_FIRST(&app_ch_head);
        LIST_REMOVE (lch, entry);
        free(lch);
    }
}

struct nvm_ftl_ops app_ops = {
    .init_ch     = app_init_channel,
    .submit_io   = app_submit_io,
    .callback_io = app_callback_io,
    .exit        = app_exit,
    .get_bbtbl   = app_ftl_get_bbtbl,
    .set_bbtbl   = app_ftl_set_bbtbl,
};

struct nvm_ftl app_ftl = {
    .ftl_id         = FTL_ID_APPNVM,
    .name           = "APPNVM",
    .nq             = 8,
    .ops            = &app_ops,
    .cap            = ZERO_32FLAG,
};

int ftl_appnvm_init (void)
{
    /* AppNVM initialization */
    bbt_byte_register ();
    blk_md_register ();

    LIST_INIT(&app_ch_head);
    app_ftl.cap |= 1 << FTL_CAP_GET_BBTBL;
    app_ftl.cap |= 1 << FTL_CAP_SET_BBTBL;
    app_ftl.bbtbl_format = FTL_BBTBL_BYTE;
    return nvm_register_ftl(&app_ftl);
}