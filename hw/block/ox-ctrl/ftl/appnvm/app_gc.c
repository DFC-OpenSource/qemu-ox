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
#define APP_GC_DELAY_CH_BUSY 1000

extern uint16_t              app_nch;
static struct app_channel  **ch;
static pthread_t             check_th;
static uint8_t               stop;
static struct app_io_data ***gc_buf;
static uint16_t              buf_pg_sz, buf_oob_sz, buf_npg;

static uint32_t recycled_blks;
static uint64_t moved_sec;

extern pthread_mutex_t gc_ns_mutex;
extern pthread_spinlock_t *md_ch_spin;

pthread_mutex_t *gc_cond_mutex;
pthread_cond_t  *gc_cond;

struct gc_th_arg {
    uint16_t            tid;
    uint16_t            bufid;
    struct app_channel *lch;
};

static int gc_bucket_sort (struct app_blk_md_entry **list,
                    uint32_t list_sz, uint32_t n_buckets, uint32_t min_invalid)
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
            if (((float) bi / (float) n_buckets < APPNVM_GC_TARGET_RATE &&
                            (k >= APPNVM_GC_MAX_BLKS)) || (bi < min_invalid)) {
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
    int nblks;
    struct app_blk_md_entry *lun;
    struct app_blk_md_entry **list = NULL;
    uint32_t lun_i, blk_i, min_inv;
    float count = 0, avlb = 0, inv_rate;

    for (lun_i = 0; lun_i < lch->ch->geometry->lun_per_ch; lun_i++) {

        lun = appnvm()->md.get_fn (lch, lun_i);
        if (!lun) {
            *c = 0;
            return NULL;
        }

        for (blk_i = 0; blk_i < lch->ch->geometry->blk_per_lun; blk_i++) {

            if (!(lun[blk_i].flags & APP_BLK_MD_AVLB))
                continue;

            if (    (lun[blk_i].flags & APP_BLK_MD_USED) &&
                   !(lun[blk_i].flags & APP_BLK_MD_OPEN) &&
                     lun[blk_i].current_pg == lch->ch->geometry->pg_per_blk) {

                count++;
                list = realloc(list, sizeof(struct app_blk_md_entry *) * count);
                if (!list)
                    goto FREE;
                list[(int)count - 1] = &lun[blk_i];
            }
            avlb++;
        }
    }

    /* Compute minimum of invalid pages for targeting a block */
    min_inv = lch->ch->geometry->pg_per_blk * APPNVM_GC_TARGET_RATE;
    inv_rate = 1.0 - ((count / avlb - APPNVM_GC_THRESD) /
                                                     (1.0 - APPNVM_GC_THRESD));
    if ((count / avlb) >= APPNVM_GC_THRESD)
        min_inv *= inv_rate;

    nblks = gc_bucket_sort(list, count, lch->ch->geometry->sec_per_blk,min_inv);
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
    uint32_t pg_i;
    struct nvm_mmgr_geometry *g = lch->ch->geometry;

    for (pg_i = 0; pg_i < g->pg_per_blk; pg_i++)
        if (list[pg_i][0].ppa)
            appnvm()->md.invalidate_fn (lch, &list[pg_i][0], APP_INVALID_PAGE);
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

static uint32_t gc_move_single_sector (struct app_channel *lch,
        struct app_blk_md_entry *blk_md, struct nvm_ppa_addr **ppa_list,
        uint16_t tid, uint32_t pg_i, uint32_t sec_i, uint32_t w_ppa_off)
{
    uint32_t pl, sec;
    uint32_t w_pl, w_pg, w_sec, w_sec_i;
    uint8_t *pg_map, *data, *oob, *w_oob;

    struct nvm_ppa_addr *old_ppa;
    struct app_io_data *io, *w_io;
    struct nvm_mmgr_geometry *geo =  lch->ch->geometry;

    pg_map = &blk_md->pg_state[pg_i * geo->n_of_planes];

    /* Check if sector is valid */
    pl  = sec_i / geo->sec_per_pg;
    sec = sec_i % geo->sec_per_pg;
    if (!(pg_map[pl] & (1 << sec))) {

        w_pg    = w_ppa_off / geo->sec_per_pl_pg;
        w_sec_i = w_ppa_off % geo->sec_per_pl_pg;
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
        w_oob  = w_io->mod_oob + (geo->sec_oob_sz * geo->sec_per_pg * w_pl);

        if (data != w_io->sec_vec[w_pl][w_sec]) {
            w_io->sec_vec[w_pl][w_sec]           = data;
            w_io->sec_vec[w_pl][geo->sec_per_pg] = w_oob;
            w_io->oob_vec[w_sec_i]               = oob;
            memcpy (w_oob + (geo->sec_oob_sz * w_sec), oob, geo->sec_oob_sz);
        }

        return ++w_ppa_off;
    }

    return w_ppa_off;
}

static uint32_t gc_move_mapping_pgs (struct app_channel *lch,
        struct app_blk_md_entry *blk_md, struct nvm_ppa_addr **ppa_list,
        uint16_t tid, uint32_t w_ppa_off, uint16_t *map_pgs, uint32_t npgs)
{
    uint32_t pl_i, w_pg, pg_i, sec_i, last_off = 0;
    struct nvm_mmgr_geometry *geo =  lch->ch->geometry;
    struct app_io_data *io, *w_io;

    w_pg = w_ppa_off / geo->sec_per_pl_pg;

    if (w_ppa_off % geo->sec_per_pl_pg != 0) {

        io   = (struct app_io_data *) gc_buf[tid][w_pg];
        w_io = (struct app_io_data *) gc_buf[tid][w_pg + npgs];

        for (pl_i = 0; pl_i < geo->n_of_planes; pl_i++) {
            memcpy (w_io->sec_vec[pl_i], io->sec_vec[pl_i],
                                    sizeof (uint8_t *) * geo->sec_per_pg + 1);
            memcpy (w_io->oob_vec, io->oob_vec,
                                    sizeof (uint8_t *) * geo->sec_per_pl_pg);
        }

        memcpy (w_io->mod_oob, io->mod_oob, geo->pg_oob_sz);
        memcpy (ppa_list[w_pg + npgs], ppa_list[w_pg],
                           sizeof (struct nvm_ppa_addr) * geo->sec_per_pl_pg);

        last_off = w_ppa_off % geo->sec_per_pl_pg;
        w_ppa_off -= last_off;
    }

    for (pg_i = 0; pg_i < npgs; pg_i++) {
        for (sec_i = 0; sec_i < geo->sec_per_pl_pg; sec_i++)
            w_ppa_off = gc_move_single_sector (lch, blk_md, ppa_list, tid,
                                              map_pgs[pg_i], sec_i, w_ppa_off);
    }

    return w_ppa_off + last_off;
}

static int gc_move_buf_sectors (struct app_channel *lch,
        struct app_blk_md_entry *blk_md, struct nvm_ppa_addr **ppa_list,
        uint16_t tid)
{
    uint32_t pg_i, sec_i;
    uint32_t ppa_off = 0, map_i = 0;

    struct app_io_data *io;
    struct nvm_mmgr_geometry *geo =  lch->ch->geometry;
    struct app_pg_oob *pg_oob;
    uint16_t map_pgs[geo->pg_per_blk];

    memset (map_pgs, 0, sizeof (uint16_t) * geo->pg_per_blk);

    for (pg_i = 0; pg_i < geo->pg_per_blk; pg_i++) {
        if (gc_check_valid_pg (lch, blk_md, pg_i)) {

            /* Buffer mapping table pages to keep page alignment */
            io = (struct app_io_data *) gc_buf[tid][pg_i];
            pg_oob = (struct app_pg_oob *) io->oob_vec[0];
            if (pg_oob->pg_type == APP_PG_MAP) {
                map_pgs[map_i] = pg_i;
                map_i++;
                continue;
            }

            for (sec_i = 0; sec_i < geo->sec_per_pl_pg; sec_i++)
                ppa_off = gc_move_single_sector
                            (lch, blk_md, ppa_list, tid, pg_i, sec_i, ppa_off);
        }
    }

    if (map_i)
        ppa_off = gc_move_mapping_pgs
                         (lch, blk_md, ppa_list, tid, ppa_off, map_pgs, map_i);

    return ppa_off;
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
                    appnvm()->md.invalidate_fn
                             (lch, &ppa_list[pg_i][sec_i], APP_INVALID_SECTOR);
                    ppa_list[pg_i][sec_i].ppa = 0x0;
                }
            }
        }

        io = (struct app_io_data *) gc_buf[tid][pg_i];

        if (app_pg_io (lch, MMGR_WRITE_SGL,
                                       (void **) io->sec_vec, ppa_list[pg_i])) {

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
    int ret = 0;

    pg_map = &blk_md->pg_state[old_ppa->g.pg * lch->ch->geometry->n_of_planes];

    if (!(pg_map[old_ppa->g.pl] & (1 << old_ppa->g.sec))) {

        switch (oob->pg_type) {
            case APP_PG_MAP:

                if (new_ppa->g.sec == 0 && new_ppa->g.pl == 0)
                    if (appnvm()->gl_map.upsert_md_fn (oob->lba, new_ppa->ppa,
                                                                 old_ppa->ppa))
                        goto INVALID;

                return 0;

            case APP_PG_NAMESPACE:

                pthread_mutex_lock (&gc_ns_mutex);
                read_ppa.ppa = appnvm()->gl_map.read_fn (oob->lba);

                if (read_ppa.ppa != old_ppa->ppa) {
                    pthread_mutex_unlock (&gc_ns_mutex);
                    goto INVALID;
                }

                ret = appnvm()->gl_map.upsert_fn (oob->lba, new_ppa->ppa);
                pthread_mutex_unlock (&gc_ns_mutex);

                if (ret)
                    goto INVALID;

                old_ppa->ppa = read_ppa.ppa;

                return 0;

            case APP_PG_PADDING:
                goto INVALID;

            default:
                log_info ("[gc: Unknown data type (%d). LBA %lu, PPA "
                        "(%d/%d/%d/%d/%d/%d)\n", oob->pg_type, oob->lba,
                        old_ppa->g.ch, old_ppa->g.lun, old_ppa->g.blk,
                        old_ppa->g.pl, old_ppa->g.pg, old_ppa->g.sec);
        }
    }

INVALID:
    appnvm()->md.invalidate_fn (lch, new_ppa, APP_INVALID_SECTOR);
    old_ppa->ppa = 0x0;
    return ret;
}

static void gc_rollback_upserted_sectors (struct nvm_ppa_addr **ppa_list,
                                                   uint32_t nsec, uint16_t tid)
{
    uint32_t sec, pg;
    struct nvm_ppa_addr *old_ppa;
    struct nvm_mmgr_geometry *geo = ch[0]->ch->geometry;
    struct app_io_data *io;
    struct app_pg_oob *oob;

    while (nsec) {
        nsec--;
        pg  = nsec / geo->sec_per_pl_pg;
        sec = nsec % geo->sec_per_pl_pg;
        old_ppa = &ppa_list[pg][geo->sec_per_pl_pg + sec];
        io   = (struct app_io_data *) gc_buf[tid][pg];
        oob  = (struct app_pg_oob *) io->oob_vec[sec];

        if (old_ppa->ppa)
            if (appnvm()->gl_map.upsert_fn (oob->lba, old_ppa->ppa))
                log_err ("[gc: Failed to rollback upserted LBAs. "
                            "LBA: %lu, old_ppa: %lx]", oob->lba, old_ppa->ppa);
    }
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
    gc_rollback_upserted_sectors (ppa_list, sec_i, tid);
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

        for (pg_i = 0; pg_i < geo->pg_per_blk; pg_i++)
            app_pg_io_prepare (lch, gc_buf[tid][pg_i]);

        /* Read all valid pages in the blk from NVM to the buffer */
        if (gc_read_blk (lch, list[blk_i], tid)) {
            log_err ("[appnvm (gc): Read block failed.]");
            continue;
        }

        /* Write all valid sectors from the buffer to new NVM PPAs */
        blk_sec = gc_write_blk (lch, list[blk_i], ppa_list, tid);
        if (blk_sec < 0) {
            log_err ("[appnvm (gc): Write block failed.]");
            continue;
        }

        /* Upsert all valid sectors in the mapping table */
        if (gc_upsert_blk_map (lch, list[blk_i], ppa_list, blk_sec, tid)) {
            log_err ("[appnvm (gc): Mapping upsert failed.]");
            gc_invalidate_err_pgs (lch, ppa_list);
            continue;
        }

        /* Put the block back in the channel provisioning */
        if (appnvm()->ch_prov.put_blk_fn (lch, list[blk_i]->ppa.g.lun,
                                                     list[blk_i]->ppa.g.blk)) {
            log_err ("[appnvm (gc): Put block failed.]");
            gc_rollback_upserted_sectors (ppa_list, blk_sec, tid);
            gc_invalidate_err_pgs (lch, ppa_list);
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

    while (!stop) {

        pthread_mutex_lock(&gc_cond_mutex[th_arg->tid]);
        pthread_cond_signal(&gc_cond[th_arg->tid]);
        pthread_cond_wait(&gc_cond[th_arg->tid], &gc_cond_mutex[th_arg->tid]);
        pthread_mutex_unlock(&gc_cond_mutex[th_arg->tid]);
        if (stop)
            break;

        appnvm_ch_active_unset (lch);

        do {
            if (!appnvm_ch_nthreads (lch)) {
                loop = 0;

                list = gc_get_target_blks (lch, &victims);
                if (!list || !victims) {
                    usleep (APP_GC_DELAY_US);
                    break;
                }

                recycled = gc_recycle_blks (list, victims,
                                                      th_arg->bufid, &blk_sec);
                if (recycled != victims)
                    log_info ("[appnvm (gc): %d recycled, %d with errors.]",
                                                 recycled, victims - recycled);

                if (APP_GC_DEBUG)
                    printf (" GC (%d): (%d/%d) %.2f MB - Total: (%d/%lu) "
                            "%.2f MB\n", lch->app_ch_id, recycled, blk_sec,
                        (4.0 * (double) blk_sec) / (double) 1024,
                        recycled_blks, moved_sec,
                        (4.0 * (double) moved_sec) / (double) 1024);
            } else {
                usleep (APP_GC_DELAY_CH_BUSY);
            }
        } while (loop);

        appnvm_ch_need_gc_unset (lch);
        if (victims)
            appnvm()->ch_prov.check_gc_fn (lch);
        appnvm_ch_active_set (lch);
    };

    return NULL;
}

static void *gc_check_fn (void *arg)
{
    uint8_t wait[app_nch];
    uint16_t cch = 0, sleep = 0, n_run = 0, ch_i, th_i;
    pthread_t run_th[app_nch];
    struct gc_th_arg *th_arg;

    memset (wait, 0x0, sizeof (uint8_t) * app_nch);

    th_arg = malloc (sizeof (struct gc_th_arg) * app_nch);
    if (!th_arg)
        return NULL;

    for (th_i = 0; th_i < app_nch; th_i++) {
        th_arg[th_i].tid = th_i;
        th_arg[th_i].lch = ch[th_i];

        if (pthread_create (&run_th[th_i], NULL, gc_run_ch,
                                                       (void *) &th_arg[th_i]))
            goto STOP;
    }

    while (!stop) {
        if (appnvm_ch_need_gc (ch[cch])) {

            th_arg[cch].bufid = n_run;
            n_run++;
            sleep++;
            wait[cch]++;

            pthread_mutex_lock (&gc_cond_mutex[cch]);
            pthread_cond_signal(&gc_cond[cch]);
            pthread_mutex_unlock (&gc_cond_mutex[cch]);

            if (n_run == APP_GC_PARALLEL_CH || (cch == app_nch - 1)) {
                for (ch_i = 0; ch_i < app_nch; ch_i++) {
                    if (wait[ch_i]) {
                        pthread_mutex_lock (&gc_cond_mutex[cch]);
                        pthread_cond_signal(&gc_cond[cch]);
                        pthread_cond_wait(&gc_cond[cch], &gc_cond_mutex[cch]);
                        pthread_mutex_unlock (&gc_cond_mutex[cch]);
                        wait[cch] = 0x0;
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

STOP:
    while (th_i) {
        th_i--;
        pthread_mutex_lock (&gc_cond_mutex[th_i]);
        pthread_cond_signal(&gc_cond[th_i]);
        pthread_mutex_unlock (&gc_cond_mutex[th_i]);
        pthread_join (run_th[th_i], NULL);
    }

    free (th_arg);
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

    gc_cond = malloc (sizeof (pthread_cond_t) * app_nch);
    if (!gc_cond)
        goto FREE_CH;

    gc_cond_mutex = malloc (sizeof (pthread_mutex_t) * app_nch);
    if (!gc_cond_mutex)
        goto FREE_COND;

    for (ch_i = 0; ch_i < app_nch; ch_i++) {
        if (pthread_cond_init (&gc_cond[ch_i], NULL))
            goto COND_MUTEX;
        if (pthread_mutex_init (&gc_cond_mutex[ch_i], NULL)) {
            pthread_cond_destroy (&gc_cond[ch_i]);
            goto COND_MUTEX;
        }
    }

    if (gc_alloc_buf ())
        goto COND_MUTEX;

    stop = 0;
    if (pthread_create (&check_th, NULL, gc_check_fn, NULL))
        goto FREE_BUF;

    return 0;

FREE_BUF:
    gc_free_buf ();
COND_MUTEX:
    while (ch_i) {
        ch_i--;
        pthread_cond_destroy (&gc_cond[ch_i]);
        pthread_mutex_destroy (&gc_cond_mutex[ch_i]);
    }
    free (gc_cond_mutex);
FREE_COND:
    free (gc_cond);
FREE_CH:
    free (ch);
    return -1;
}

static void gc_exit (void)
{
    uint16_t ch_i;

    stop++;
    pthread_join (check_th, NULL);
    gc_free_buf ();

    for (ch_i = 0; ch_i < app_nch; ch_i++) {
        pthread_cond_destroy (&gc_cond[ch_i]);
        pthread_mutex_destroy (&gc_cond_mutex[ch_i]);
    }

    free (gc_cond_mutex);
    free (gc_cond);
    free (ch);
}

void gc_register (void)
{
    appnvm()->gc.init_fn = gc_init;
    appnvm()->gc.exit_fn = gc_exit;
}