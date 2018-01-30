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

#define APP_GC_DEBUG         1
#define APP_GC_PARALLEL_CH   1
#define APP_GC_DELAY_US      10000
#define APP_GC_DELAY_CH_BUSY 5000

extern uint16_t              app_nch;
static struct app_channel  **ch;
static pthread_t             check_th;
static uint8_t               stop;
static struct app_io_data ***gc_buf;
static uint16_t              buf_pg_sz, buf_oob_sz, buf_npg;

static uint32_t recycled_blks;
static uint64_t moved_sec;

extern pthread_mutex_t gc_ns_mutex;
extern pthread_mutex_t gc_map_mutex;
extern pthread_spinlock_t *md_ch_spin;

struct gc_th_arg {
    uint16_t            tid;
    struct app_channel *lch;
};

static int gc_bucket_sort (struct app_blk_md_entry **list,
                                        uint32_t list_sz, uint32_t n_buckets)
{
    uint32_t stop, bi, j, k, ip, bucketc[++n_buckets];
    struct app_blk_md_entry **bucket[n_buckets];
    int ret = -1;

    memset (bucketc, 0, n_buckets * sizeof (uint32_t));
    memset (bucket, 0, n_buckets * sizeof (struct app_blk_md_entry *));

    for(j = 0; j < list_sz; j++) {;
        ip = list[j]->invalid_sec;
        if (ip) {
            bucketc[ip]++;
            bucket[ip] = realloc (bucket[ip],
                              bucketc[ip] * sizeof (struct app_blk_md_entry *));
            if (!bucket[ip])
                goto FREE;
            bucket[ip][bucketc[ip] - 1] = list[j];
        }
    }

    k = 0;
    stop = 0;
    for (bi = n_buckets - 1; bi > 0; bi--) {
        for (j = 0; j < bucketc[bi]; j++) {
            if ((float) bi / (float) n_buckets <
                            APPNVM_GC_TARGET_RATE && k >= APPNVM_GC_MAX_BLKS) {
                stop++;
                break;
            }
            list[k] = bucket[bi][j];
            k++;
        }
        if (stop) break;
    }
    ret = k;

FREE:
    for(bi = 0; bi < n_buckets; bi++)
        if (bucket[bi])
            free (bucket[bi]);
    return ret;
}

static struct app_blk_md_entry **gc_get_target_blks (struct app_channel *lch,
                                                                   uint32_t *c)
{
    uint32_t lun_i, blk_i, count = 0;
    int nblks;
    struct app_blk_md_entry *lun;
    struct app_blk_md_entry **list = NULL;

    for (lun_i = 0; lun_i < lch->ch->geometry->lun_per_ch; lun_i++) {

        lun = appnvm()->md.get_fn (lch, lun_i);
        if (!lun) {
            *c = 0;
            return NULL;
        }

        for (blk_i = 0; blk_i < lch->ch->geometry->blk_per_lun; blk_i++) {

            if (    (lun[blk_i].flags & APP_BLK_MD_USED) &&
                   !(lun[blk_i].flags & APP_BLK_MD_OPEN) &&
                     lun[blk_i].current_pg == lch->ch->geometry->pg_per_blk) {

                count++;
                list = realloc(list, sizeof(struct app_blk_md_entry *) * count);
                if (!list)
                    goto FREE;
                list[count - 1] = &lun[blk_i];
            }
        }
    }

    nblks = gc_bucket_sort (list, count, lch->ch->geometry->sec_per_blk);
    if (nblks < 0)
        goto FREE;

    *c = nblks;
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
    uint8_t *pg_map;
    uint8_t off;
    uint32_t pg_i, pl_i;
    struct app_blk_md_entry *lun;
    struct nvm_mmgr_geometry *g = lch->ch->geometry;

    off = (1 << g->sec_per_pg) - 1;

    for (pg_i = 0; pg_i < g->pg_per_blk; pg_i++) {

        if (list[pg_i][0].ppa) {
            lun = appnvm()->md.get_fn (lch, list[pg_i][0].g.lun);
            pg_map = &lun[list[pg_i][0].g.blk].pg_state[pg_i * g->n_of_planes];

            pthread_spin_lock (&md_ch_spin[lch->app_ch_id]);

            /* Set the sectors as invalid in all planes */
            for (pl_i = 0; pl_i < g->n_of_planes; pl_i++)
                pg_map[pl_i] = off;

            lun[list[pg_i][0].g.blk].invalid_sec += g->sec_per_pl_pg;

            pthread_spin_unlock (&md_ch_spin[lch->app_ch_id]);
        }
    }
}

static int gc_check_valid_pg (struct app_channel *lch,
                                  struct app_blk_md_entry *blk_md, uint16_t pg)
{
    uint8_t *pg_map;
    uint8_t map, off, pl;

    pg_map = &blk_md->pg_state[pg * lch->ch->geometry->n_of_planes];
    map    = lch->ch->geometry->n_of_planes;
    off    = (1 << lch->ch->geometry->sec_per_pg) - 1;

    for (pl = 0; pl < lch->ch->geometry->n_of_planes; pl++)
        if ((pg_map[pl] & off) == off)
            map--;

    return map;
}

static int gc_read_blk (struct app_channel *lch,
                                 struct app_blk_md_entry *blk_md, uint16_t tid)
{
    uint32_t pg_i;
    struct nvm_ppa_addr ppa;
    struct app_io_data *io;

    for (pg_i = 0; pg_i < lch->ch->geometry->pg_per_blk; pg_i++) {

        if (gc_check_valid_pg (lch, blk_md, pg_i)) {
            ppa.ppa = 0x0;
            ppa.ppa = blk_md->ppa.ppa;
            ppa.g.pg = pg_i;

            io = (struct app_io_data *) gc_buf[tid][pg_i];

            if (app_pg_io (lch, MMGR_READ_PG, (void **) io->pl_vec, &ppa))
                return -1;
        }
    }

    return 0;
}

static int gc_move_buf_sectors (struct app_channel *lch,
        struct app_blk_md_entry *blk_md, struct nvm_ppa_addr **ppa_list,
        uint16_t tid)
{
    uint32_t pg_i, sec_i, pl, sec;
    uint32_t w_pl, w_pg, w_sec, w_sec_i, ppa_off = 0;
    uint8_t *pg_map, *data, *w_data, *oob, *w_oob;

    struct nvm_ppa_addr *old_ppa;
    struct app_io_data *io, *w_io;
    struct nvm_mmgr_geometry *geo =  lch->ch->geometry;

    for (pg_i = 0; pg_i < geo->pg_per_blk; pg_i++) {
        if (gc_check_valid_pg (lch, blk_md, pg_i)) {

            for (sec_i = 0; sec_i < geo->sec_per_pl_pg; sec_i++) {
                pg_map = &blk_md->pg_state[pg_i * geo->n_of_planes];

                /* Check if sector is valid */
                pl  = sec_i / geo->sec_per_pg;
                sec = sec_i % geo->sec_per_pg;
                if (!(pg_map[pl] & (1 << sec))) {

                    w_pg    = ppa_off / geo->sec_per_pl_pg;
                    w_sec_i = ppa_off % geo->sec_per_pl_pg;
                    w_sec   = w_sec_i % geo->sec_per_pg;
                    w_pl    = w_sec_i / geo->sec_per_pg;

                    /* Set old ppa list */
                    old_ppa = &ppa_list[w_pg][geo->sec_per_pl_pg + w_sec_i];
                    old_ppa->ppa   = blk_md->ppa.ppa;
                    old_ppa->g.pg  = pg_i;
                    old_ppa->g.pl  = pl;
                    old_ppa->g.sec = sec;

                    /* Move sector data and oob to write position */
                    io   = (struct app_io_data *) gc_buf[tid][pg_i];
                    data = io->pl_vec[pl] + (sec * geo->sec_size);
                    oob  = io->oob_vec[sec_i];

                    w_io   = (struct app_io_data *) gc_buf[tid][w_pg];
                    w_data = w_io->pl_vec[w_pl] + (w_sec * geo->sec_size);
                    w_oob  = w_io->oob_vec[w_sec_i];

                    if (data != w_data) {
                        memcpy (w_data, data, geo->sec_size);
                        memcpy (w_oob, oob, geo->sec_oob_sz);
                    }

                    ppa_off++;
                }
            }
        }
    }

    return ppa_off;
}

static void gc_invalidate_sector (struct nvm_ppa_addr *ppa)
{
    struct app_blk_md_entry *lun;
    uint8_t *pg_map;

    lun = appnvm()->md.get_fn (ch[ppa->g.ch], ppa->g.lun);
    pg_map = &lun[ppa->g.blk].pg_state
                        [ppa->g.pg * ch[ppa->g.ch]->ch->geometry->n_of_planes];

    pthread_spin_lock (&md_ch_spin[ppa->g.ch]);
    pg_map[ppa->g.pl] |= 1 << ppa->g.sec;
    lun[ppa->g.blk].invalid_sec++;
    pthread_spin_unlock (&md_ch_spin[ppa->g.ch]);
}

static int gc_write_blk (struct app_channel *lch, struct app_blk_md_entry *md,
                                  struct nvm_ppa_addr **ppa_list, uint16_t tid)
{
    uint32_t sec_i, pg_i, npgs, valid_sec = 0;
    struct nvm_mmgr_geometry *geo =  lch->ch->geometry;
    struct app_io_data *io;

    for (pg_i = 0; pg_i < geo->pg_per_blk; pg_i++)
        for (sec_i = 0; sec_i < geo->sec_per_pl_pg * 2; sec_i++)
            ppa_list[pg_i][sec_i].ppa = 0x0;

    valid_sec = gc_move_buf_sectors (lch, md, ppa_list, tid);

    npgs = valid_sec / geo->sec_per_pl_pg;
    if (valid_sec % geo->sec_per_pl_pg != 0)
        npgs++;

    for (pg_i = 0; pg_i < npgs; pg_i++) {

        /* Write page to the same channel to keep the parallelism */
        if (appnvm()->ch_prov.get_ppas_fn (lch, ppa_list[pg_i], 1)) {
            ppa_list[pg_i][0].ppa = 0x0;
            gc_invalidate_err_pgs (lch, ppa_list);
            return -1;
        }

        /* Invalidate padded sectors in the last page */
        if (pg_i == npgs - 1) {
            for (sec_i = 0; sec_i < geo->sec_per_pl_pg; sec_i++) {
                if (!ppa_list[pg_i][geo->sec_per_pl_pg + sec_i].ppa) {
                    ppa_list[pg_i][sec_i].ppa = 0x0;
                    gc_invalidate_sector (&ppa_list[pg_i][sec_i]);
                }
            }
        }

        io = (struct app_io_data *) gc_buf[tid][pg_i];

        if (app_pg_io (lch, MMGR_WRITE_PG,
                                       (void **) io->pl_vec, ppa_list[pg_i])) {

            /* TODO: If write fails, tell provisioning to recycle
             * current block and abort any write to the blocks,
             * otherwise we loose the blk sequential writes guarantee */

            gc_invalidate_err_pgs (lch, ppa_list);
            return -1;
        }
    }

    return valid_sec;
}

static int gc_upsert_sector (struct app_channel *lch,
        struct app_blk_md_entry *blk_md, struct nvm_ppa_addr *new_ppa,
        struct nvm_ppa_addr *old_ppa, struct app_pg_oob *oob)
{
    uint8_t *pg_map;
    struct nvm_ppa_addr read_ppa;
    int ret = oob->lba;

    pg_map = &blk_md->pg_state[old_ppa->g.pg * lch->ch->geometry->n_of_planes];

    if (!(pg_map[old_ppa->g.pl] & (1 << old_ppa->g.sec))) {

        switch (oob->pg_type) {
            case APP_PG_MAP:

                log_info (" GC MAP PAGE UPDATE\n");
                return 0;

            case APP_PG_NAMESPACE:

                pthread_mutex_lock (&gc_ns_mutex);
                read_ppa.ppa = appnvm()->gl_map.read_fn (oob->lba);

                if (read_ppa.ppa != old_ppa->ppa) {
                    pthread_mutex_unlock (&gc_ns_mutex);
                    ret = 0;
                    goto INVALID;
                }

                ret = appnvm()->gl_map.upsert_fn (oob->lba, new_ppa->ppa);
                pthread_mutex_unlock (&gc_ns_mutex);

                if (ret)
                    goto INVALID;

                old_ppa->ppa = read_ppa.ppa;

                return 0;

            default:
                ret = 0;
                goto INVALID;
        }

    } else {
        ret = 0;
        goto INVALID;
    }

INVALID:
    gc_invalidate_sector (new_ppa);
    old_ppa->ppa = 0x0;
    return ret;
}

static int gc_upsert_blk_map (struct app_channel *lch,
            struct app_blk_md_entry *blk_md, struct nvm_ppa_addr **ppa_list,
            uint32_t sectors, uint32_t tid)
{
    struct nvm_mmgr_geometry *geo = lch->ch->geometry;
    struct nvm_ppa_addr *new_ppa, *old_ppa;
    struct app_io_data *io;
    struct app_pg_oob *oob;

    uint32_t pg, sec, sec_i;
    uint64_t lba;

    for (sec_i = 0; sec_i < sectors; sec_i++) {
        pg  = sec_i / geo->sec_per_pl_pg;
        sec = sec_i % geo->sec_per_pl_pg;

        new_ppa = &ppa_list[pg][sec];
        old_ppa = &ppa_list[pg][geo->sec_per_pl_pg + sec];
        io   = (struct app_io_data *) gc_buf[tid][pg];
        oob  = (struct app_pg_oob *) io->oob_vec[sec];

        lba = gc_upsert_sector (lch, blk_md, new_ppa, old_ppa, oob);
        if (lba)
            goto ROLLBACK;
    }

    return 0;

ROLLBACK:
    while (sec_i) {
        sec_i--;
        pg  = sec_i / geo->sec_per_pl_pg;
        sec = sec_i % geo->sec_per_pl_pg;
        old_ppa = &ppa_list[pg][geo->sec_per_pl_pg + sec];
        io   = (struct app_io_data *) gc_buf[tid][pg];
        oob  = (struct app_pg_oob *) io->oob_vec[sec];

        if (old_ppa->ppa)
            if (appnvm()->gl_map.upsert_fn (oob->lba, old_ppa->ppa))
                log_err ("[gc: Failed to rollback upserted LBAs. "
                            "LBA: %lu, old_ppa: %lx]", oob->lba, old_ppa->ppa);
    }

    return -1;
}

static int gc_recycle_blks (struct app_blk_md_entry **list, uint32_t count,
                                                uint16_t tid, uint32_t *ch_sec)
{
    int blk_sec;
    uint32_t blk_i, pg_i, recycled = 0, count_sec = 0;
    struct app_channel *lch;
    struct nvm_mmgr_geometry *geo = ch[0]->ch->geometry;

    /* This list is used to store the old and new PPA of collected sectors */
    struct nvm_ppa_addr *ppa_list[geo->pg_per_blk];

    for (pg_i = 0; pg_i < geo->pg_per_blk; pg_i++) {
        ppa_list[pg_i] = malloc (sizeof (struct nvm_ppa_addr)
                                                     * geo->sec_per_pl_pg * 2);
        if (!ppa_list[pg_i])
            goto FREE_PPA;
    }

    for (blk_i = 0; blk_i < count; blk_i++) {
        lch = ch[list[blk_i]->ppa.g.ch];

        /* Read all invalid pages in the blk from NVM to the buffer */
        if (gc_read_blk (lch, list[blk_i], tid)) {
            log_err ("[appnvm (gc): Read block failed.]");
            continue;
        }

        /* Write all invalid pages from the buffer to new NVM PPAs */
        blk_sec = gc_write_blk (lch, list[blk_i], ppa_list, tid);
        if (blk_sec < 0) {
            log_err ("[appnvm (gc): Write block failed.]");
            continue;
        }

        if (gc_upsert_blk_map (lch, list[blk_i], ppa_list, blk_sec, tid)) {
            log_err ("[appnvm (gc): Mapping upsert failed.]");
            gc_invalidate_err_pgs (lch, ppa_list);
            continue;
        }

        if (appnvm()->ch_prov.put_blk_fn (lch, list[blk_i]->ppa.g.lun,
                                                     list[blk_i]->ppa.g.blk)) {
            log_err ("[appnvm (gc): Put block failed.]");
            gc_invalidate_err_pgs (lch, ppa_list);
            /* TODO: Return upserted sectors to previous state */
            continue;
        }

        recycled++;
        recycled_blks++;
        moved_sec += blk_sec;
        count_sec += blk_sec;
    }

FREE_PPA:
    while (pg_i) {
        pg_i--;
        free (ppa_list[pg_i]);
    }
    *ch_sec = count_sec;
    return recycled;
}

static void *gc_run_ch (void *arg)
{
    uint8_t  loop = 1;
    uint32_t victims, recycled, blk_sec;

    struct gc_th_arg         *th_arg = (struct gc_th_arg *) arg;
    struct app_channel       *lch = th_arg->lch;
    struct app_blk_md_entry **list;

    appnvm_ch_active_unset (lch);

    do {
        if (!appnvm_ch_nthreads (lch)) {
            loop = 0;

            list = gc_get_target_blks (lch, &victims);
            if (!list || !victims) {
                usleep (APP_GC_DELAY_US);
                break;
            }

            recycled = gc_recycle_blks (list, victims, th_arg->tid, &blk_sec);
            if (recycled != victims)
                log_info ("[appnvm (gc): %d recycled, %d with errors.]",
                                                 recycled, victims - recycled);

            if (APP_GC_DEBUG)
                printf ("\nGC ch %d: (%d/%d) %lu MB - Total: (%d/%lu) %lu MB\n",
                   lch->app_ch_id, recycled, blk_sec,
                   (lch->ch->geometry->sec_size * (uint64_t) blk_sec) / 1024,
                   recycled_blks, moved_sec,
                   (lch->ch->geometry->sec_size * (uint64_t) moved_sec) / 1024);
        } else {
            usleep (APP_GC_DELAY_CH_BUSY);
        }
    } while (loop);

    appnvm_ch_need_gc_unset (lch);
    appnvm()->ch_prov.check_gc_fn (lch);
    appnvm_ch_active_set (lch);

    free (th_arg);

    return NULL;
}

static void *gc_check_fn (void *arg)
{
    uint16_t cch = 0, sleep = 0, n_run = 0, ch_i;
    pthread_t run_th[app_nch];
    struct gc_th_arg *th_arg;

    memset (run_th, 0x0, sizeof (pthread_t) * app_nch);

    while (!stop) {

        if (appnvm_ch_need_gc (ch[cch])) {

            th_arg = malloc (sizeof (struct gc_th_arg));
            if (!th_arg)
                continue;

            th_arg->tid = n_run;
            th_arg->lch = ch[cch];

            if (pthread_create (&run_th[cch], NULL, gc_run_ch, (void *) th_arg))
                continue;

            n_run++;
            sleep++;

            if (n_run == APP_GC_PARALLEL_CH || (cch == app_nch - 1)) {
                for (ch_i = 0; ch_i < app_nch; ch_i++) {
                    if (run_th[ch_i]) {
                        pthread_join (run_th[ch_i], NULL);
                        run_th[ch_i] = 0x0;
                        n_run--;
                    }
                    if (!n_run)
                        break;
                }

            }
        }

        cch = (cch == app_nch - 1) ? 0 : cch + 1;
        if (!cch) {
            if (!sleep)
                usleep (APP_GC_DELAY_US);
            sleep = 0;
        }
    }

    return NULL;
}

static int gc_alloc_buf (void)
{
    uint32_t th_i, pg_i;

    gc_buf = malloc (sizeof (void *) * APP_GC_PARALLEL_CH);
    if (!gc_buf)
        return -1;

    for (th_i = 0; th_i < APP_GC_PARALLEL_CH; th_i++) {
        gc_buf[th_i] = malloc (sizeof (uint8_t *) * buf_npg);
        if (!gc_buf[th_i])
            goto FREE_BUF;

        for (pg_i = 0; pg_i < buf_npg; pg_i++) {
            gc_buf[th_i][pg_i] = app_alloc_pg_io (ch[0]);
            if (!gc_buf[th_i][pg_i])
                goto FREE_BUF_PG;
        }
    }

    return 0;

FREE_BUF_PG:
    while (pg_i) {
        pg_i--;
        app_free_pg_io (gc_buf[th_i][pg_i]);
    }
    free (gc_buf[th_i]);
FREE_BUF:
    while (th_i) {
        th_i--;
        for (pg_i = 0; pg_i < buf_npg; pg_i++)
            app_free_pg_io (gc_buf[th_i][pg_i]);
        free (gc_buf[th_i]);
    }
    free (gc_buf);
    return -1;
}

static void gc_free_buf (void)
{
    uint32_t pg_i, th_i = APP_GC_PARALLEL_CH;

    while (th_i) {
        th_i--;
        for (pg_i = 0; pg_i < buf_npg; pg_i++)
            app_free_pg_io (gc_buf[th_i][pg_i]);
        free (gc_buf[th_i]);
    }
    free (gc_buf);
}

static int gc_init (void)
{
    uint16_t nch, ch_i;
    struct nvm_mmgr_geometry *geo;

    ch = malloc (sizeof (struct app_channel *) * app_nch);
    if (!ch)
        return -1;

    nch = appnvm()->channels.get_list_fn (ch, app_nch);
    if (nch != app_nch)
        goto FREE_CH;

    recycled_blks = 0;
    moved_sec = 0;

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

    if (gc_alloc_buf ())
        goto FREE_CH;

    stop = 0;
    if (pthread_create (&check_th, NULL, gc_check_fn, NULL))
        goto FREE_BUF;

    return 0;

FREE_BUF:
    gc_free_buf ();
FREE_CH:
    free (ch);
    return -1;
}

static void gc_exit (void)
{
    stop++;
    pthread_join (check_th, NULL);
    gc_free_buf ();
    free (ch);
}

void gc_register (void)
{
    appnvm()->gc.init_fn = gc_init;
    appnvm()->gc.exit_fn = gc_exit;
}