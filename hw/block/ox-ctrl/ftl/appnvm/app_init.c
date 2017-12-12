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

LIST_HEAD(app_ch, app_channel) app_ch_head = LIST_HEAD_INITIALIZER(app_ch_head);

static int app_submit_io (struct nvm_io_cmd *);

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

static int app_init_channel (struct nvm_channel *ch)
{
    uint32_t tblks;
    int ret, trsv, n, pl, n_pl;
    struct app_channel *lch;
    struct app_bbtbl *bbt;
    struct nvm_ppa_addr *ppa;

    n_pl = ch->geometry->n_of_planes;

    ch->ftl_rsv = APP_RSV_BLK_COUNT;
    trsv = ch->ftl_rsv * n_pl;
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

    lch = malloc (sizeof(struct app_channel));
    if (!lch)
        return EMEM;

    tblks = ch->geometry->blk_per_lun * ch->geometry->lun_per_ch * n_pl;

    lch->ch = ch;

    ret = EMEM;
    lch->bbtbl = malloc (sizeof(struct app_bbtbl));
    if (!lch->bbtbl)
        goto FREE_LCH;

    bbt = lch->bbtbl;
    bbt->tbl = malloc (sizeof(uint8_t) * tblks);
    if (!bbt->tbl)
        goto FREE_BBTBL;

    memset (bbt->tbl, 0, tblks);
    bbt->magic = 0;
    bbt->bb_sz = tblks;

    ret = app_get_bbt_nvm(lch, bbt);
    if (ret) goto ERR;

    /* create and flush bad block table if it does not exist */
    /* this procedure will erase the entire device (only in test mode) */
    if (bbt->magic == APP_MAGIC) {
        printf(" [appnvm: Channel %d. Creating bad block table...]", ch->ch_id);
        fflush(stdout);
        ret = app_bbt_create (lch, bbt, APP_BBT_EMERGENCY);
        if (ret) goto ERR;
        ret = app_flush_bbt (lch, bbt);
        if (ret) goto ERR;
    }

    LIST_INSERT_HEAD(&app_ch_head, lch, entry);
    log_info("    [appnvm: channel %d started with %d bad blocks.]\n",ch->ch_id,
                                                                bbt->bb_count);

    return 0;

ERR:
    free(bbt->tbl);
FREE_BBTBL:
    free(bbt);
FREE_LCH:
    free(lch);
    log_err("[appnvm ERR: Ch %d -> Not possible to read/create bad block "
                                                        "table.]\n", ch->ch_id);
    return ret;
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
        ret = app_flush_bbt (lch, lch->bbtbl);
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

struct nvm_ftl app = {
    .ftl_id         = FTL_ID_APPNVM,
    .name           = "APPNVM",
    .nq             = 8,
    .ops            = &app_ops,
    .cap            = ZERO_32FLAG,
};

int ftl_appnvm_init (void)
{
    LIST_INIT(&app_ch_head);
    app.cap |= 1 << FTL_CAP_GET_BBTBL;
    app.cap |= 1 << FTL_CAP_SET_BBTBL;
    app.bbtbl_format = FTL_BBTBL_BYTE;
    return nvm_register_ftl(&app);
}