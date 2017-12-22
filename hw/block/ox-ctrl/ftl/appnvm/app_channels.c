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
#include <sys/queue.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include "hw/block/ox-ctrl/include/ssd.h"

LIST_HEAD(app_ch, app_channel) app_ch_head = LIST_HEAD_INITIALIZER(app_ch_head);

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

    /* For bad block table we consider blocks in a single plane */
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

    ret = appnvm()->bbt.load_fn (lch);
    if (ret) goto ERR;

    /* create and flush bad block table if it does not exist */
    /* this procedure will erase the entire device (only in test mode) */
    if (bbt->magic == APP_MAGIC) {
        ret = appnvm()->bbt.create_fn (lch, APP_BBT_EMERGENCY);
        if (ret) goto ERR;
        ret = appnvm()->bbt.flush_fn (lch);
        if (ret) goto ERR;
    }

    log_info("    [appnvm: Bad Block Table started. Ch %d]\n", ch->ch_id);

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

    /* For block metadata, we consider multi-plane blocks */
    tblks = ch->geometry->blk_per_lun * ch->geometry->lun_per_ch;

    lch->blk_md = malloc (sizeof(struct app_blk_md));
    if (!lch->blk_md)
        return -1;

    md = lch->blk_md;
    md->entry_sz = sizeof(struct app_blk_md_entry);
    md->tbl = malloc (md->entry_sz * tblks);
    if (!md->tbl)
        goto FREE_MD;

    memset (md->tbl, 0, md->entry_sz * tblks);
    md->magic = 0;
    md->entries = tblks;

    ret = appnvm()->md.load_fn (lch);
    if (ret) goto ERR;

    /* create and flush block metadata table if it does not exist */
    if (md->magic == APP_MAGIC) {
        ret = appnvm()->md.create_fn (lch);
        if (ret) goto ERR;
        ret = appnvm()->md.flush_fn (lch);
        if (ret) goto ERR;
    }

    log_info("    [appnvm: Block Metadata started. Ch %d]\n", ch->ch_id);

    return 0;

ERR:
    free(lch->blk_md->tbl);
FREE_MD:
    free(lch->blk_md);
    return -1;
}

static int channels_init (struct nvm_channel *ch, uint16_t id)
{
    struct app_channel *lch;

    lch = malloc (sizeof(struct app_channel));
    if (!lch)
        return EMEM;

    lch->ch = ch;

    LIST_INSERT_HEAD(&app_ch_head, lch, entry);
    lch->app_ch_id = id;

    if (app_reserve_blks (lch))
        goto FREE_LCH;

    if (app_init_bbt (lch))
        goto FREE_LCH;

    if (app_init_blk_md (lch))
        goto FREE_LCH;

    if (appnvm()->ch_prov.init_fn (lch))
        goto FREE_LCH;

    log_info("    [appnvm: channel %d started with %d bad blocks.]\n",ch->ch_id,
                                                          lch->bbtbl->bb_count);
    return 0;

FREE_LCH:
    LIST_REMOVE (lch, entry);
    free(lch);
    log_err("[appnvm ERR: Ch %d -> Not possible to read/create bad block "
                                                        "table.]\n", ch->ch_id);
    return EMEM;
}

static void channels_exit (struct app_channel *lch)
{
    int ret, retry;
    
    retry = 0;
    do {
        retry++;
        ret = appnvm()->md.flush_fn (lch);
    } while (ret && retry < APPNVM_FLUSH_RETRY);

    /* TODO: Recover from last checkpoint (make a checkpoint) */
    if (ret)
        log_err(" [appnvm: ERROR. Block metadata not flushed to NVM. "
                                          "Channel %d]", lch->ch->ch_id);
    else
        log_info(" [appnvm: Block metadata persisted into NVM. "
                                          "Channel %d]", lch->ch->ch_id);

    appnvm()->ch_prov.exit_fn (lch);
    free(lch->blk_md->tbl);
    free(lch->blk_md);
    free(lch->bbtbl->tbl);
    free(lch->bbtbl);
     
    LIST_REMOVE (lch, entry);
    free(lch);
}

static struct app_channel *channels_get(uint16_t ch_id)
{
    struct app_channel *lch;
    LIST_FOREACH(lch, &app_ch_head, entry){
        if(lch->ch->ch_id == ch_id)
            return lch;
    }

    return NULL;
}

static int channels_get_list (struct app_channel **list, uint16_t nch)
{
    int n = 0;
    struct app_channel *lch;

    LIST_FOREACH(lch, &app_ch_head, entry){
        if (n >= nch)
            break;
        list[n] = lch;
        n++;
    }

    return n;
}

void channels_register (void) {
    appnvm()->channels.init_fn = channels_init;
    appnvm()->channels.exit_fn = channels_exit;
    appnvm()->channels.get_fn = channels_get;
    appnvm()->channels.get_list_fn = channels_get_list;

    LIST_INIT(&app_ch_head);
}