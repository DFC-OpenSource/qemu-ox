/* OX: Open-Channel NVM Express SSD Controller
 *  - AppNVM Garbage Collecting
 *
 * Copyright 2018 IT University of Copenhagen.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 *
 * Partially supported by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 */

#include <stdlib.h>
#include <stdio.h>
#include "appnvm.h"
#include <sys/queue.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include "hw/block/ox-ctrl/include/ssd.h"

#define APP_GC_PARALLEL_CH   1
#define APP_GC_CHECK_US      10000

extern uint16_t             app_nch;
static struct app_channel **ch;
static pthread_t            check_th;
static uint8_t              stop;
static uint8_t            **gc_buf;
static uint16_t             buf_pg_sz, buf_oob_sz, buf_npg;

static struct app_blk_md_entry **gc_get_target_blks (struct app_channel *lch, uint32_t *c)
{
    uint32_t lun_i, blk_i, count = 0/*, count2*/;
    struct app_blk_md_entry *lun;
    struct app_blk_md_entry **list = NULL;

    for (lun_i = 0; lun_i < lch->ch->geometry->lun_per_ch; lun_i++) {
        lun = appnvm()->md.get_fn (lch, lun_i);
        if (!lun)
            return NULL;

        for (blk_i = 0; blk_i < lch->ch->geometry->blk_per_lun; blk_i++) {

            /*printf ("ch %d lun %d blk %d - inv %d era %d cur_pg %d\n", lun[blk_i].ppa.g.ch, lun[blk_i].ppa.g.lun, lun[blk_i].ppa.g.blk,
                    lun[blk_i].invalid_pgs, lun[blk_i].erase_count, lun[blk_i].current_pg);
            if (lun[blk_i].invalid_pgs > 0) {
                count2 = 0;
                for (int pg_i = 0; pg_i < lch->ch->geometry->pg_per_blk; pg_i++) {
                    if (lun[blk_i].pg_state[pg_i / 8] & (1 << (pg_i % 8)))
                        count2++;
                    printf (" [pg %d - %d]\n", pg_i, lun[blk_i].pg_state[pg_i / 8] & (1 << (pg_i % 8)));
                }
                printf (" invalid count %d\n", count2);
            }*/

            if ((lun[blk_i].flags & APP_BLK_MD_USED) &&
                      lun[blk_i].current_pg == lch->ch->geometry->pg_per_blk) {

                /* TODO: get victims by invalid pages rate calculated by
                         real device utilization. For now, Victims have 50%
                         invalid or more */
                if (lun[blk_i].invalid_pgs >= lch->ch->geometry->pg_per_blk / 2) {
                    count++;
                    list = realloc (list, sizeof (struct app_blk_md_entry *) * count);
                    if (!list)
                        goto FREE;
                    list[count - 1] = &lun[blk_i];
                }
            }
        }
    }
    *c = count;
    return list;

FREE:
    if (count > 1)
        free (list);
    *c = 0;
    return NULL;
}

static void gc_invalidate_err_pgs (struct app_channel *lch,
                                                    struct nvm_ppa_addr **list)
{
    uint32_t pg_i;
    struct app_blk_md_entry *lun;

    for (pg_i = 0; pg_i < lch->ch->geometry->pg_per_blk; pg_i++) {

        if (list[pg_i][0].ppa) {
            lun = appnvm()->md.get_fn (lch, list[pg_i][0].g.lun);

            /* Set the page as invalid */
            lun[list[pg_i][0].g.blk].pg_state[pg_i / 8] |= 1 << (pg_i % 8);
            lun[list[pg_i][0].g.blk].invalid_pgs++;
        }
    }
}

static int gc_read_blk (struct app_channel *lch,struct app_blk_md_entry *blk_md)
{
    uint32_t pg_i, pl_i;
    struct nvm_ppa_addr ppa;
    uint8_t *vec[lch->ch->geometry->n_of_planes];

    for (pg_i = 0; pg_i < lch->ch->geometry->pg_per_blk; pg_i++) {

        /* Check if page is valid */
        if (!(blk_md->pg_state[pg_i / 8] & (1 << pg_i % 8))) {
            ppa.ppa = 0x0;
            ppa.ppa = blk_md->ppa.ppa;
            ppa.g.pg = pg_i;

            for (pl_i = 0; pl_i < lch->ch->geometry->n_of_planes; pl_i++)
                vec[pl_i] = gc_buf[pg_i] + ((buf_pg_sz + buf_oob_sz) * pl_i);

            if (app_pg_io (lch, MMGR_READ_PG, (void **) vec, &ppa))
                return -1;
            uint64_t *lba = (uint64_t *)(vec[0] + buf_pg_sz);
            uint64_t *lba2 = (uint64_t *)(vec[0] + buf_pg_sz + lch->ch->geometry->sec_oob_sz);
            printf ("  R page %d ok, LBA1 %lu, LBA2 %lu\n", pg_i, *lba, *lba2);
        }
    }

    return 0;
}

static int gc_write_blk (struct app_channel *lch,
               struct app_blk_md_entry *blk_md, struct nvm_ppa_addr **ppa_list)
{
    uint32_t pg_i, sec_i, pl_i;
    struct nvm_mmgr_geometry *geo =  lch->ch->geometry;
    void *vec[lch->ch->geometry->n_of_planes];

    for (pg_i = 0; pg_i < geo->pg_per_blk; pg_i++)
        for (sec_i = 0; sec_i < geo->sec_per_pl_pg; sec_i++)
            ppa_list[pg_i][sec_i].ppa = 0x0;

    for (pg_i = 0; pg_i < geo->pg_per_blk; pg_i++) {

        /* Check if page is valid */
        if (!(blk_md->pg_state[pg_i / 8] & (1 << pg_i % 8))) {

            /* Write page to the same channel to keep the same parallelism */
            if (appnvm()->ch_prov.get_ppas_fn (lch, ppa_list[pg_i], 1)) {
                ppa_list[pg_i][0].ppa = 0x0;
                gc_invalidate_err_pgs (lch, ppa_list);
                return -1;
            }

            for (pl_i = 0; pl_i < lch->ch->geometry->n_of_planes; pl_i++)
                vec[pl_i] = gc_buf[pg_i] + ((buf_pg_sz + buf_oob_sz) * pl_i);

            if (app_pg_io (lch, MMGR_WRITE_PG, vec, &ppa_list[pg_i][0])) {

                /* TODO: If write fails, tell provisioning to recycle
                 * current block and abort any write to the blocks,
                 * otherwise we loose the blk sequential writes guarantee */

                gc_invalidate_err_pgs (lch, ppa_list);
                return -1;
            }
            uint64_t *lba = (uint64_t *)(vec[0] + buf_pg_sz);
            uint64_t *lba2 = (uint64_t *)(vec[0] + buf_pg_sz + lch->ch->geometry->sec_oob_sz);
            printf ("  W page %d ok, LBA1 %lu, LBA2 %lu\n", pg_i, *lba, *lba2);
        }
    }

    return 0;
}

static int gc_upsert_blk_map (struct app_channel *lch,
               struct app_blk_md_entry *blk_md, struct nvm_ppa_addr **ppa_list)
{
    uint32_t pg_i, sec_i, pl_i;
    uint64_t *lba;
    struct nvm_mmgr_geometry *geo =  lch->ch->geometry;
    void *vec[lch->ch->geometry->n_of_planes];

    for (pg_i = 0; pg_i < geo->pg_per_blk; pg_i++) {

        /* Check if page is valid */
        if (!(blk_md->pg_state[pg_i / 8] & (1 << pg_i % 8))) {
            for (sec_i = 0; sec_i < geo->sec_per_pl_pg; sec_i++) {

                for (pl_i = 0; pl_i < lch->ch->geometry->n_of_planes; pl_i++)
                    vec[pl_i] = gc_buf[pg_i] + ((buf_pg_sz + buf_oob_sz) * pl_i);

                lba = (uint64_t *) (vec[sec_i / geo->sec_per_pg] + buf_pg_sz +
                                 (geo->sec_oob_sz * (sec_i % geo->sec_per_pg)));

                struct nvm_ppa_addr ppa;
                ppa.ppa = appnvm()->gl_map.read_fn (*lba);

                printf ("  (%d/%d/%d/%d/%d/%d) LBA %lu\n",
                        ppa_list[pg_i][sec_i].g.ch,
                        ppa_list[pg_i][sec_i].g.lun,
                        ppa_list[pg_i][sec_i].g.blk,
                        ppa_list[pg_i][sec_i].g.pl,
                        ppa_list[pg_i][sec_i].g.pg,
                        ppa_list[pg_i][sec_i].g.sec, *lba);

                printf (   "OLD_PPA (%d/%d/%d/%d), READ_PPA (%d/%d/%d/%d/%d/%d)\n", blk_md->ppa.g.ch, blk_md->ppa.g.lun, blk_md->ppa.g.blk, pg_i,
                        ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);

                if (appnvm()->gl_map.upsert_fn (*lba, ppa_list[pg_i][sec_i].ppa))
                    goto ROLLBACK;

            }
        }
    }

    return 0;

ROLLBACK:
    /* TODO: Return upserted sectors to previous state */
    return -1;
}

static int gc_recycle_blks (struct app_blk_md_entry **list, uint32_t count)
{
    uint32_t blk_i, pg_i, recycled = 0;
    struct app_channel *lch;
    struct nvm_mmgr_geometry *geo = ch[0]->ch->geometry;
    struct nvm_ppa_addr *ppa_list[geo->pg_per_blk];

    for (pg_i = 0; pg_i < geo->pg_per_blk; pg_i++) {
        ppa_list[pg_i] = malloc (sizeof (struct nvm_ppa_addr)
                                                         * geo->sec_per_pl_pg);
        if (!ppa_list[pg_i])
            goto FREE_PPA;
    }

    for (blk_i = 0; blk_i < count; blk_i++) {
        lch = ch[list[blk_i]->ppa.g.ch];

        printf ("\nRECYCLE: (%d/%d/%d). Invalid: %d\n",
                list[blk_i]->ppa.g.ch, list[blk_i]->ppa.g.lun, list[blk_i]->ppa.g.blk, list[blk_i]->invalid_pgs);

        printf (" READING...\n");
        /* Read all invalid pages in the blk from NVM to the buffer */
        if (gc_read_blk (lch, list[blk_i])) {
            log_err ("[appnvm (gc): Read block failed.]");
            continue;
        }

        printf (" WRITING...\n");
        /* Write all invalid pages from the buffer to new NVM PPAs */
        if (gc_write_blk (lch, list[blk_i], ppa_list)) {
            log_err ("[appnvm (gc): Write block failed.]");
            continue;
        }

        printf (" UPSERTING...\n");
        if (gc_upsert_blk_map (lch, list[blk_i], ppa_list)) {
            log_err ("[appnvm (gc): Mapping upsert failed.]");
            gc_invalidate_err_pgs (lch, ppa_list);
            continue;
        }

        printf (" PUT BLK...\n");
        if (appnvm()->ch_prov.put_blk_fn (lch, list[blk_i]->ppa.g.lun,
                                                     list[blk_i]->ppa.g.blk)) {
            log_err ("[appnvm (gc): Put block failed.]");
            gc_invalidate_err_pgs (lch, ppa_list);
            /* TODO: Return upserted sectors to previous state */
            continue;
        }
        printf (" OK!\n");

        recycled++;
    }

FREE_PPA:
    while (pg_i) {
        pg_i--;
        free (ppa_list[pg_i]);
    }
    return recycled;
}

static void *gc_run_ch (void *arg)
{
    uint8_t loop = 1;
    uint32_t victims, recycled;
    struct app_channel *lch = (struct app_channel *) arg;
    struct app_blk_md_entry **list;

    appnvm_ch_active_unset (lch);

    printf ("CH %d has started GC!\n", lch->app_ch_id);

    do {
        if (!appnvm_ch_nthreads (lch)) {
            loop = 0;
            list = gc_get_target_blks (lch, &victims);
            if (!list) {
                log_err ("[appnvm (gc): Target blocks not selected. Ch %d]",
                                                                lch->app_ch_id);
                continue;
            }
            printf (" Total victims: %d\n", victims);

            recycled = gc_recycle_blks (list, victims);
            if (recycled != victims)
                log_info ("[appnvm (gc): %d recycled, %d with errors.]",
                                                 recycled, victims - recycled);
            printf (" Recycled: %d\n", recycled);
        } else
            usleep (1);
    } while (loop);

    sleep (5);
    appnvm_ch_need_gc_unset (lch);
    printf ("CH %d has finished GC!\n", lch->app_ch_id);

    return NULL;
}

static void *gc_check_fn (void *arg)
{
    uint16_t cch = 0, sleep = 0, n_run = 0;
    pthread_t run_th[app_nch];

    memset (run_th, 0x0, sizeof (pthread_t) * app_nch);

    while (!stop) {

        if (appnvm_ch_need_gc (ch[cch])) {
            if (pthread_create (&run_th[cch], NULL, gc_run_ch, (void *)ch[cch]))
                continue;
            n_run++;
            sleep++;

            if (n_run == APP_GC_PARALLEL_CH || cch == app_nch - 1) {
                while (n_run) {
                    n_run--;
                    pthread_join (run_th[n_run], NULL);
                }
            }
        }

        cch = (cch == app_nch - 1) ? 0 : cch + 1;
        if (!cch) {
            if (!sleep)
                usleep (APP_GC_CHECK_US);
            sleep = 0;
        }
    }

    return NULL;
}

static int gc_init (void)
{
    uint16_t nch, ch_i, pg_i;
    struct nvm_mmgr_geometry *geo;

    ch = malloc (sizeof (struct app_channel *) * app_nch);
    if (!ch)
        return -1;

    nch = appnvm()->channels.get_list_fn (ch, app_nch);
    if (nch != app_nch)
        goto FREE_CH;

    geo = ch[0]->ch->geometry;
    buf_pg_sz = geo->pg_size;
    buf_oob_sz = geo->pg_oob_sz;
    buf_npg = geo->pg_per_blk;

    for (ch_i = 0; ch_i < app_nch; ch_i++) {
        geo = ch[ch_i]->ch->geometry;
        buf_pg_sz = MAX(geo->pg_size, buf_pg_sz);
        buf_oob_sz = MAX(geo->pg_oob_sz, buf_oob_sz);
        buf_npg = MAX(geo->pg_per_blk, buf_npg);
    }

    gc_buf = malloc (sizeof (uint8_t *) * buf_npg);
    if (!gc_buf)
        goto FREE_CH;

    for (pg_i = 0; pg_i < buf_npg; pg_i++) {
        gc_buf[pg_i] = calloc ((buf_pg_sz + buf_oob_sz) * geo->n_of_planes, 1);
        if (!gc_buf[pg_i])
            goto FREE_BUF;
    }

    stop = 0;
    if (pthread_create (&check_th, NULL, gc_check_fn, NULL))
        goto FREE_BUF;

    return 0;

FREE_BUF:
    while (pg_i) {
        pg_i--;
        free (gc_buf[pg_i]);
    }
    free (gc_buf);
FREE_CH:
    free (ch);
    return -1;
}

static void gc_exit (void)
{
    uint32_t pg_i;

    stop++;
    pthread_join (check_th, NULL);

    for (pg_i = 0; pg_i < buf_npg; pg_i++)
        free (gc_buf[pg_i]);
    free (gc_buf);

    free (ch);
}

void gc_register (void)
{
    appnvm()->gc.init_fn = gc_init;
    appnvm()->gc.exit_fn = gc_exit;
}