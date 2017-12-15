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
#include <search.h>
#include <sys/queue.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include "hw/block/ox-ctrl/include/ssd.h"

struct ch_prov_blk {
    struct nvm_ppa_addr             addr;
    struct app_blk_md_entry         *blk_md;
    uint8_t                         *state;
    CIRCLEQ_ENTRY(ch_prov_blk)      entry;
};

struct ch_prov_lun {
    struct nvm_ppa_addr     addr;
    struct ch_prov_blk      *vblks;
    uint32_t                nfree_blks;
    uint32_t                nused_blks;
    pthread_mutex_t         l_mutex;
    CIRCLEQ_HEAD(free_blk_list, ch_prov_blk) free_blk_head;
    CIRCLEQ_HEAD(used_blk_list, ch_prov_blk) used_blk_head;
    CIRCLEQ_HEAD(open_blk_list, ch_prov_blk) open_blk_head;
};

struct ch_prov_line {
    uint16_t              nblks;
    struct ch_prov_blk  **vblks;
};

struct ch_prov {
    struct ch_prov_lun  *luns;
    struct ch_prov_blk  **prov_vblks;
    struct ch_prov_line *line;
};

static struct ch_prov_blk *ch_prov_blk_rand (struct app_channel *lch,
                                                                       int lun)
{
    int blk, blk_idx;
    struct ch_prov_blk *vblk, *tmp;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    if (prov->luns[lun].nfree_blks > 0) {
        blk_idx = rand() % prov->luns[lun].nfree_blks;
        vblk = CIRCLEQ_FIRST(&(prov->luns[lun].free_blk_head));

        for (blk = 0; blk < blk_idx; blk++) {
            tmp = CIRCLEQ_NEXT(vblk, entry);
            vblk = tmp;
        }

        return vblk;
    }
    return NULL;
}

static int ch_prov_blk_alloc(struct app_channel *lch, int lun, int blk)
{
    int pl;
    int bad_blk = 0;
    struct ch_prov_blk *rnd_vblk;
    int n_pl = lch->ch->geometry->n_of_planes;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    struct ch_prov_blk *vblk = &(prov->prov_vblks[lun][blk]);

    uint8_t *bbt = appnvm()->bbt.get_fn (lch, lun);

    vblk->state = malloc(sizeof (uint8_t) * n_pl);
    if (vblk->state == NULL)
        return -1;

    vblk->addr = prov->luns[lun].addr;
    vblk->addr.g.blk = blk;
    vblk->blk_md = &appnvm()->md.get_fn (lch, lun)[blk];

    for (pl = 0; pl < lch->ch->geometry->n_of_planes; pl++) {
        vblk->state[pl] = bbt[n_pl * blk + pl];
        bad_blk += vblk->state[pl];
    }

    if (!bad_blk) {

        if (vblk->blk_md->flags & APP_BLK_MD_USED) {
             CIRCLEQ_INSERT_HEAD(&(prov->luns[lun].used_blk_head), vblk, entry);

            if (vblk->blk_md->flags & APP_BLK_MD_OPEN)
                CIRCLEQ_INSERT_HEAD(&(prov->luns[lun].open_blk_head),
                                                                   vblk, entry);
             return 0;
        }

        rnd_vblk = ch_prov_blk_rand(lch, lun);

        if (rnd_vblk == NULL) {
            CIRCLEQ_INSERT_HEAD(&(prov->luns[lun].free_blk_head), vblk, entry);
        } else {

            if (rand() % 2)
                CIRCLEQ_INSERT_BEFORE(&(prov->luns[lun].free_blk_head),
                                                rnd_vblk, vblk, entry);
            else
                CIRCLEQ_INSERT_AFTER(&(prov->luns[lun].free_blk_head),
                                                rnd_vblk, vblk, entry);
        }
        prov->luns[lun].nfree_blks++;
    }

    return 0;
}

static int ch_prov_list_create(struct app_channel *lch, int lun)
{
    int blk;
    int nblk;
    struct nvm_ppa_addr addr;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    addr.ppa = 0x0;
    addr.g.ch = lun / lch->ch->geometry->lun_per_ch;
    addr.g.lun = lun % lch->ch->geometry->lun_per_ch;
    prov->luns[lun].addr = addr;
    nblk = lch->ch->geometry->blk_per_lun;

    prov->luns[lun].nfree_blks = 0;
    prov->luns[lun].nused_blks = 0;
    CIRCLEQ_INIT(&(prov->luns[lun].free_blk_head));
    CIRCLEQ_INIT(&(prov->luns[lun].used_blk_head));
    CIRCLEQ_INIT(&(prov->luns[lun].open_blk_head));
    pthread_mutex_init(&(prov->luns[lun].l_mutex), NULL);

    for (blk = 0; blk < nblk; blk++)
        ch_prov_blk_alloc(lch, lun, blk);

    return 0;
}

static int ch_prov_blk_free (struct app_channel *lch, int lun, int blk)
{
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    free (prov->prov_vblks[lun][blk].state);

    return 0;
}


static void ch_prov_blk_list_free(struct app_channel *lch, int lun)
{
    int nblk;
    int blk;
    struct ch_prov_blk *vblk, *tmp;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    nblk = lch->ch->geometry->blk_per_lun;

    if (prov->luns[lun].nfree_blks > 0) {
        vblk = CIRCLEQ_FIRST(&(prov->luns[lun].free_blk_head));

        for (blk = 0; blk < prov->luns[lun].nfree_blks; blk++) {
            tmp = CIRCLEQ_NEXT(vblk, entry);
            CIRCLEQ_REMOVE(&(prov->luns[lun].free_blk_head), vblk, entry);
            vblk = tmp;
        }
    }

    if (prov->luns[lun].nused_blks > 0) {
        vblk = CIRCLEQ_FIRST(&(prov->luns[lun].used_blk_head));

        for (blk = 0; blk < prov->luns[lun].nused_blks; blk++) {
            tmp = CIRCLEQ_NEXT(vblk, entry);
            CIRCLEQ_REMOVE(&(prov->luns[lun].used_blk_head), vblk, entry);
            vblk = tmp;
        }
    }

    for (blk = 0; blk < nblk; blk++) {
        ch_prov_blk_free(lch, lun, blk);
    }

    pthread_mutex_destroy(&(prov->luns[lun].l_mutex));
}

static int ch_prov_init_luns (struct app_channel *lch)
{
    int lun, err_lun;
    int nluns;
    int nblocks;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    srand(time(NULL));

    nluns = lch->ch->geometry->lun_per_ch;
    nblocks = lch->ch->geometry->blk_per_lun;

    prov->luns = malloc (sizeof (struct ch_prov_lun) *
                                                lch->ch->geometry->lun_per_ch);
    if (!prov->luns)
        return -1;

    prov->prov_vblks = malloc(nluns * sizeof(struct ch_prov_blk *));
    if (!prov->prov_vblks)
        goto FREE_LUNS;

    for (lun = 0; lun < nluns; lun++) {
        prov->prov_vblks[lun] = malloc(nblocks * sizeof (struct ch_prov_blk));
        if (!prov->prov_vblks[lun]) {
            for (err_lun = 0; err_lun < lun; err_lun++)
                free (prov->prov_vblks[err_lun]);
            goto FREE_VBLKS;
        }

        if (ch_prov_list_create(lch, lun) < 0) {
            for (err_lun = 0; err_lun < lun; err_lun++)
                ch_prov_blk_list_free(lch, err_lun);
            goto FREE_VBLKS_LUN;
        }
    }

    return 0;

  FREE_VBLKS_LUN:
    for (err_lun = 0; err_lun < nluns; err_lun++)
        free (prov->prov_vblks[err_lun]);

  FREE_VBLKS:
    free (prov->prov_vblks);

  FREE_LUNS:
    free (prov->luns);
    return -1;
}

static int ch_prov_exit_luns (struct app_channel *lch)
{
    int lun;
    int nluns;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    nluns = lch->ch->geometry->lun_per_ch;

    for (lun = 0; lun < nluns; lun++) {
        ch_prov_blk_list_free(lch, lun);
        free (prov->prov_vblks[lun]);
    }

    free (prov->prov_vblks);
    free (prov->luns);

    return 0;
}

static int ch_prov_renew_line (struct app_channel *lch)
{
    /* TODO: Check open blocks list, if not enough, get a block and check
     again until enough blocks are open. Then, set the blk IDs in the line */

    return 0;
}

static struct ch_prov_blk *ch_prov_blk_get (struct app_channel *lch, int lun)
{
    int ret, i;
    int n_pl = lch->ch->geometry->n_of_planes;
    struct nvm_mmgr_io_cmd *cmd;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    struct ch_prov_lun *p_lun = &prov->luns[lun];

NEXT:
    pthread_mutex_lock(&(p_lun->l_mutex));

    if (p_lun->nfree_blks > 0) {

        struct ch_prov_blk *vblk = CIRCLEQ_FIRST(&p_lun->free_blk_head);

        CIRCLEQ_REMOVE(&(p_lun->free_blk_head), vblk, entry);
        CIRCLEQ_INSERT_TAIL(&(p_lun->used_blk_head), vblk, entry);
        CIRCLEQ_INSERT_HEAD(&(p_lun->open_blk_head), vblk, entry);

        p_lun->nfree_blks--;
        p_lun->nused_blks++;

        pthread_mutex_unlock(&(p_lun->l_mutex));

        /* Erase the block, if it fails, mark as bad and try next block */
        cmd = malloc (sizeof(struct nvm_mmgr_io_cmd) * n_pl);
        if (!cmd)
            return NULL;

        ret = nvm_submit_multi_plane_sync_io (lch->ch, cmd, NULL,
                                                            MMGR_ERASE_BLK, 0);
        if (ret) {
            for (i = 0; i < n_pl; i++)
                lch->ch->ftl->ops->set_bbtbl (&vblk->addr, NVM_BBT_BAD);

            pthread_mutex_lock(&(p_lun->l_mutex));
            CIRCLEQ_REMOVE(&(p_lun->used_blk_head), vblk, entry);
            CIRCLEQ_REMOVE(&(p_lun->open_blk_head), vblk, entry);
            p_lun->nused_blks--;
            pthread_mutex_unlock(&(p_lun->l_mutex));

            free (cmd);
            goto NEXT;
        }
        free (cmd);

        vblk->blk_md->current_pg = 0;
        vblk->blk_md->flags |= (APP_BLK_MD_USED | APP_BLK_MD_OPEN);
        memset (vblk->blk_md->pg_state, 0x0, 64);

        return vblk;
    }

    pthread_mutex_unlock(&(p_lun->l_mutex));

    return NULL;
}

static int ch_prov_blk_put(struct app_channel *lch, uint16_t lun, uint16_t blk)
{
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    struct ch_prov_lun *p_lun = &prov->luns[lun];
    struct ch_prov_blk *vblk = &prov->prov_vblks[lun][blk];

    if (!(vblk->blk_md->flags & APP_BLK_MD_USED))
        return -1;

    if (vblk->blk_md->flags & APP_BLK_MD_OPEN)
        return -1;

    vblk->blk_md->flags ^= APP_BLK_MD_USED;

    pthread_mutex_lock(&(p_lun->l_mutex));
    CIRCLEQ_REMOVE(&(p_lun->used_blk_head), &prov->prov_vblks[lun][blk], entry);
    CIRCLEQ_INSERT_TAIL(&(p_lun->free_blk_head),
                                           &prov->prov_vblks[lun][blk], entry);
    p_lun->nfree_blks++;
    p_lun->nused_blks--;
    pthread_mutex_unlock(&(p_lun->l_mutex));

    return 0;
}

static int ch_prov_init (struct app_channel *lch)
{
    struct ch_prov *prov = malloc (sizeof (struct ch_prov));
    if (!prov)
        return -1;

    lch->ch_prov = prov;
    if (ch_prov_init_luns (lch))
        goto ERR;

    ch_prov_renew_line (lch);

    return 0;

ERR:
    free (prov);
    return -1;
}

static void ch_prov_exit (struct app_channel *lch)
{
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    ch_prov_exit_luns (lch);
    free (prov->luns);
}

static struct nvm_ppa_addr *ch_prov_get_ppas (struct app_channel *lch,
                                                                uint16_t pgs)
{
    /* TODO: Get ppas from the line as round-robin. If a block is full,
     get a new block and renew the line */

    return NULL;
}

void blk_ch_prov_register (void) {
    appnvm()->ch_prov.init_fn = ch_prov_init;
    appnvm()->ch_prov.exit_fn = ch_prov_exit;
    appnvm()->ch_prov.put_blk_fn = ch_prov_blk_put;
    appnvm()->ch_prov.get_ppas_fn = ch_prov_get_ppas;
}