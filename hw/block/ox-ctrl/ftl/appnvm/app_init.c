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

LIST_HEAD(app_ch, app_channel) ch_head = LIST_HEAD_INITIALIZER(ch_head);

static int app_submit_io (struct nvm_io_cmd *);

static struct app_channel *app_get_ch_instance(uint16_t ch_id)
{
    struct app_channel *lch;
    LIST_FOREACH(lch, &ch_head, entry){
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
    return 0;
}

static int app_init_channel (struct nvm_channel *ch)
{
    return 0;
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

    LIST_FOREACH(lch, &ch_head, entry){
        free(lch->bbtbl->tbl);
        free(lch->bbtbl);
    }
    while (!LIST_EMPTY(&ch_head)) {
        lch = LIST_FIRST(&ch_head);
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
    .ftl_id         = FTL_ID_APP,
    .name           = "APPNVM",
    .nq             = 8,
    .ops            = &app_ops,
    .cap            = ZERO_32FLAG,
};

int app_init (void)
{
    LIST_INIT(&ch_head);
    app.cap |= 1 << FTL_CAP_GET_BBTBL;
    app.cap |= 1 << FTL_CAP_SET_BBTBL;
    app.bbtbl_format = FTL_BBTBL_BYTE;
    return nvm_register_ftl(&app);
}