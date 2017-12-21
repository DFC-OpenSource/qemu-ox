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
static uint8_t gl_fn; /* Positive if global function has been called */

LIST_HEAD(app_ch, app_channel) app_ch_head = LIST_HEAD_INITIALIZER(app_ch_head);
uint16_t app_nch;

static int app_submit_io (struct nvm_io_cmd *);

struct app_global *appnvm (void) {
    return &__appnvm;
}

struct app_io_data *app_alloc_pg_io (struct app_channel *lch)
{
    struct app_io_data *data;

    data = malloc (sizeof (struct app_io_data));
    if (!data)
        return NULL;

    data->lch = lch;
    data->ch = lch->ch;
    data->n_pl = data->ch->geometry->n_of_planes;
    data->pg_sz = data->ch->geometry->pg_size;
    data->meta_sz = data->ch->geometry->sec_oob_sz *
                                                data->ch->geometry->sec_per_pg;
    data->buf_sz = (data->pg_sz + data->meta_sz) * data->n_pl;

    data->buf = calloc(data->buf_sz, 1);
    if (!data->buf) {
        free (data);
        return NULL;
    }

    return data;
}

void app_free_pg_io (struct app_io_data *data)
{
    free (data->buf);
    free (data);
}

int app_blk_current_page (struct app_channel *lch, struct app_io_data *io,
                                                               uint16_t offset)
{
    int i, pg, ret, fr = 0;
    struct app_magic oob;

    if (io == NULL) {
        io = app_alloc_pg_io(lch);
        if (io == NULL)
            return -1;
        fr++;
    }

    uint8_t *buf_vec[io->n_pl];

    for (i = 0; i < io->n_pl; i++)
        buf_vec[i] = io->buf + i * (io->buf_sz / io->n_pl);

    /* Finds the location of the newest data by checking APP_MAGIC */
    pg = 0;
    do {
        memset (io->buf, 0, io->buf_sz);
        ret = app_io_rsv_blk (lch, MMGR_READ_PG, (void **) buf_vec,
                                                            lch->meta_blk, pg);

        /* get info from OOB area in plane 0 */
        memcpy(&oob, io->buf + io->pg_sz, sizeof(struct app_magic));

        if (ret || oob.magic != APP_MAGIC)
            break;

        pg += offset;
    } while (pg < io->ch->geometry->pg_per_blk - offset);

    if (fr)
        app_free_pg_io (io);

    return (!ret) ? pg : -1;
}

/**
 *  Transfers a table of user specific entries to/from a NVM reserved block
 *  This function mount/unmount the multi-plane I/O buffers to a flat table
 *
 *  The table can cross multiple pages in the block
 *  For now, the maximum table size is the flash block
 *
 * @param io - struct created by alloc_alloc_pg_io
 * @param user_buf - table buffer to be transfered
 * @param pgs - number of flash pages the table crosses (multi-plane pages)
 * @param start_pg - first page to be written
 * @param ent_per_pg - number of entries per flash page (multi-plane pages)
 * @param ent_left - number of entries to be transfered
 * @param entry_sz - size of an entry
 * @param blk_id - reserved block ID (for now, in LUN 0)
 * @param direction - APP_TRANS_FROM_NVM or APP_TRANS_TO_NVM
 * @return 0 on success, -1 on failure
 */
int app_meta_transfer (struct app_io_data *io, uint8_t *user_buf,
        uint16_t pgs, uint16_t start_pg,  uint16_t ent_per_pg,
        uint32_t ent_left, size_t entry_sz, uint16_t blk_id, uint8_t direction)
{
    int i, pl;
    size_t trf_sz, pg_ent_sz;
    uint8_t *from, *to;

    uint8_t *buf_vec[io->n_pl];
    for (i = 0; i < io->n_pl; i++)
        buf_vec[i] = io->buf + i * (io->buf_sz / io->n_pl);

    pg_ent_sz = ent_per_pg * entry_sz;

    /* Transfer page by page from/to NVM */
    for (i = 0; i < pgs; i++) {

        if (direction == APP_TRANS_FROM_NVM)
            if (app_io_rsv_blk (io->lch, MMGR_READ_PG, (void **) buf_vec,
                                                        blk_id, start_pg + i))
                return -1;

        /* Copy page entries from/to I/O buffer */
        for (pl = 0; pl < io->n_pl; pl++) {

            trf_sz = (ent_left >= ent_per_pg / io->n_pl) ?
                    (ent_per_pg / io->n_pl) * entry_sz : ent_left * entry_sz;

            from = (direction == APP_TRANS_TO_NVM) ?
                user_buf + (pg_ent_sz * i) + (pl * (pg_ent_sz / io->n_pl)) :
                buf_vec[pl];

            to = (direction == APP_TRANS_TO_NVM) ?
                buf_vec[pl] :
                user_buf + (pg_ent_sz * i) + (pl * (pg_ent_sz / io->n_pl));

            memcpy(to, from, trf_sz);

            ent_left = (ent_left >= ent_per_pg / io->n_pl) ?
                ent_left - (ent_per_pg / io->n_pl) : 0;

            if (!ent_left)
                break;
        }

        if (direction == APP_TRANS_TO_NVM)
            if (app_io_rsv_blk (io->lch, MMGR_WRITE_PG, (void **) buf_vec,
                                                        blk_id, start_pg + i))
                return -1;
    }

    return 0;
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

static int app_init_channel (struct nvm_channel *ch)
{
    struct app_channel *lch;

    lch = malloc (sizeof(struct app_channel));
    if (!lch)
        return EMEM;

    lch->ch = ch;

    LIST_INSERT_HEAD(&app_ch_head, lch, entry);
    app_nch++;

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
    app_nch--;
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
        ret = appnvm()->bbt.flush_fn(lch);
        if (ret)
            log_info("[ftl WARNING: Error flushing bad block table to NVM!]");
    }

    return 0;
}

static void app_exit (struct nvm_ftl *ftl)
{
    int ret, retry;
    struct app_channel *lch;
    struct nvm_ftl_cap_gl_fn app_gl;

    LIST_FOREACH(lch, &app_ch_head, entry){
        retry = 0;
        do {
            retry++;
            ret = appnvm()->md.flush_fn (lch);
        } while (ret && retry < APPNVM_FLUSH_RETRY);

        /* TODO: Recover from last checkpoint (make a checkpoint) */
        if (ret)
            log_err("[appnvm: ERROR. Block metadata not flushed to NVM. "
                                              "Channel %d\n]", lch->ch->ch_id);

        appnvm()->ch_prov.exit_fn (lch);
        free(lch->blk_md->tbl);
        free(lch->blk_md);
        free(lch->bbtbl->tbl);
        free(lch->bbtbl);
    }

    /* APPNVM Global Exit */
    app_gl.arg = NULL;
    app_gl.ftl_id = FTL_ID_APPNVM;
    app_gl.fn_id = 0;
    nvm_ftl_cap_exec(FTL_CAP_EXIT_FN, &app_gl);

    while (!LIST_EMPTY(&app_ch_head)) {
        lch = LIST_FIRST(&app_ch_head);
        LIST_REMOVE (lch, entry);
        app_nch--;
        free(lch);
    }
}

static int app_global_init (void)
{
    if (appnvm()->flags.init_fn ()) {
        log_err ("[appnvm: Flags NOT started.\n");
        return -1;
    }

    return 0;
}

static void app_global_exit (void)
{
    appnvm()->flags.exit_fn ();
}

static int app_init_fn (uint16_t fn_id, void *arg)
{
    switch (fn_id) {
        case APP_FN_GLOBAL:
            gl_fn = 1;
            return app_global_init();
            break;
        default:
            log_info ("[appnvm (init_fn): Function not found. id %d\n", fn_id);
            return -1;
    }
}

static void app_exit_fn (uint16_t fn_id)
{
    switch (fn_id) {
        case APP_FN_GLOBAL:
            return (gl_fn) ? app_global_exit() : 0;
            break;
        default:
            log_info ("[appnvm (exit_fn): Function not found. id %d\n", fn_id);
    }
}

struct nvm_ftl_ops app_ops = {
    .init_ch     = app_init_channel,
    .submit_io   = app_submit_io,
    .callback_io = app_callback_io,
    .exit        = app_exit,
    .get_bbtbl   = app_ftl_get_bbtbl,
    .set_bbtbl   = app_ftl_set_bbtbl,
    .init_fn     = app_init_fn,
    .exit_fn     = app_exit_fn
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
    gl_fn = 0;
    app_nch = 0;

    /* AppNVM initialization */
    bbt_byte_register ();
    blk_md_register ();
    ch_prov_register ();
    flags_register ();

    LIST_INIT(&app_ch_head);
    app_ftl.cap |= 1 << FTL_CAP_GET_BBTBL;
    app_ftl.cap |= 1 << FTL_CAP_SET_BBTBL;
    app_ftl.cap |= 1 << FTL_CAP_INIT_FN;
    app_ftl.cap |= 1 << FTL_CAP_EXIT_FN;
    app_ftl.bbtbl_format = FTL_BBTBL_BYTE;
    return nvm_register_ftl(&app_ftl);
}