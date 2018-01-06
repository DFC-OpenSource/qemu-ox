/* OX: OpenChannel NVM Express SSD Controller
 *
 * Copyright (C) 2016, IT University of Copenhagen. All rights reserved.
 * Written by          Ivan Luiz Picoli <ivpi@itu.dk>
 *
 * Funding support provided by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#include <stdlib.h>
#include <stdio.h>
#include "appnvm.h"
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/queue.h>
#include "hw/block/ox-ctrl/include/ssd.h"

#define MAP_BUF_CH_PGS  10        /* 4 MB per channel */
#define MAP_BUF_PG_SZ   32 * 1024 /* 32 KB */

#define MAP_ADDR_FLAG   ((1 & AND64) << 63)

struct map_cache_entry {
    uint8_t                     dirty;
    uint8_t                    *buf;
    uint32_t                    buf_sz;
    struct nvm_ppa_addr         ppa;    /* Stores the PPA while pg is cached */
    struct app_map_entry       *md_entry;
    struct map_cache           *cache;
    pthread_mutex_t            *mutex;
    LIST_ENTRY(map_cache_entry)  f_entry;
    TAILQ_ENTRY(map_cache_entry) u_entry;
};

struct map_cache {
    struct map_cache_entry                 *pg_buf;
    LIST_HEAD(mb_free_l, map_cache_entry)   mbf_head;
    TAILQ_HEAD(mb_used_l, map_cache_entry)  mbu_head;
    pthread_spinlock_t                      mb_spin;
    uint32_t                                nfree;
    uint32_t                                nused;
    uint16_t                                id;
};

struct map_pg_addr {
    union {
        struct {
            uint64_t addr  : 63;
            uint64_t flag : 1;
        } g;
        uint64_t addr;
    };
};

static struct map_cache    *map_ch_cache;
extern uint16_t             app_nch;
static struct app_channel **ch;

/* The mapping strategy ensures the entry size matches with the NVM pg size */
static uint64_t             ent_per_pg;

/**
 * - The mapping table is spread using the global provisioning functions.
 * - The mapping table is recovered by secondary metadata table stored in a
 *    reserved block per channel. Mapping table metadata key/PPA entries are
 *    stored into multi-plane NVM pages in the reserved block using
 *    round-robin distribution among all NVM channels.
 * - Each channel has a separated cache, where only mapping table key/PPA
 *    entries belonging to this channel (previously spread by round-robin)
 *    are cached into.
 * - Cached pages are flushed back to NVM using the global provisioning. This
 *    ensures the mapping table I/Os follow the same provisioning strategy than
 *    the rest of the FTL.
 */
static int map_nvm_write (struct map_cache_entry *ent)
{
    struct app_prov_ppas *prov_ppa;
    struct app_channel *lch;
    struct nvm_ppa_addr *addr;
    struct app_io_data *io;
    int ret = -1;

    prov_ppa = appnvm()->gl_prov.get_ppa_list_fn (1);
    if (!prov_ppa) {
        log_err ("[appnvm (gl_map): I/O error. No PPAs available.]");
        return -1;
    }

    if (prov_ppa->nch != 1 ||
            prov_ppa->nppas != prov_ppa->ch[0]->ch->geometry->sec_per_pl_pg)
        log_err ("[appnvm (gl_map): NVM write. wrong PPAs. nppas %d, nchs %d]",
                                                prov_ppa->nppas, prov_ppa->nch);

    addr = &prov_ppa->ppa[0];
    lch = ch[addr->g.ch];
    io = app_alloc_pg_io(lch);
    if (io == NULL)
        goto FREE_PPA;

    ret = app_nvm_seq_transfer (io, addr, ent->buf, 1, ent_per_pg,
                            ent_per_pg, sizeof(struct app_map_entry),
                            APP_TRANS_TO_NVM, APP_IO_NORMAL);
    if (ret)
        /* TODO: If write fails, the block should be closed and subsequent
         * writes to the same block should be rescheduled */
        log_err("[appnvm (gl_map): NVM write failed. PPA 0x%016lx]", addr->ppa);

    ent->ppa.ppa = addr->ppa;
    app_free_pg_io(io);

FREE_PPA:
    appnvm()->gl_prov.free_ppa_list_fn (prov_ppa);
    return ret;
}

static int map_nvm_read (struct map_cache_entry *ent)
{
    struct app_channel *lch;
    struct nvm_ppa_addr addr;
    struct app_io_data *io;
    int ret = -1;

    addr.ppa = ent->md_entry->ppa;

    /* TODO: Support multiple media managers for mapping the channel ID */
    lch = ch[addr.g.ch];

    io = app_alloc_pg_io(lch);
    if (io == NULL)
        return -1;

    ret = app_nvm_seq_transfer (io, &addr, ent->buf, 1, ent_per_pg,
                            ent_per_pg, sizeof(struct app_map_entry),
                            APP_TRANS_FROM_NVM, APP_IO_NORMAL);
    if (ret)
        log_err("[appnvm (gl_map): NVM read failed. PPA 0x%016lx]", addr.ppa);

    ent->ppa.ppa = addr.ppa;
    app_free_pg_io(io);

    return ret;
}

static int map_evict_pg_cache (struct map_cache *cache)
{
    struct map_cache_entry *cache_ent;

    cache_ent = TAILQ_FIRST(&cache->mbu_head);
    if (!cache_ent)
        return -1;

    pthread_mutex_lock (cache_ent->mutex);

    pthread_spin_lock (&cache->mb_spin);
    TAILQ_REMOVE(&cache->mbu_head, cache_ent, u_entry);
    cache->nused--;
    pthread_spin_unlock (&cache->mb_spin);

    if (cache_ent->dirty) {
        if (map_nvm_write (cache_ent)) {

            pthread_spin_lock (&cache->mb_spin);
            TAILQ_INSERT_HEAD(&cache->mbu_head, cache_ent, u_entry);
            cache->nused++;
            pthread_spin_unlock (&cache->mb_spin);

            return -1;
        }
        cache_ent->dirty = 0;

        /* Cache entry PPA is set after the write completes */
    }

    cache_ent->md_entry->ppa = cache_ent->ppa.ppa;
    cache_ent->ppa.ppa = 0;
    cache_ent->md_entry = NULL;

    pthread_mutex_unlock (cache_ent->mutex);

    pthread_spin_lock (&cache->mb_spin);
    LIST_INSERT_HEAD (&cache->mbf_head, cache_ent, f_entry);
    cache->nfree++;
    pthread_spin_unlock (&cache->mb_spin);

    return 0;
}

static int map_load_pg_cache (struct map_cache *cache,
           struct app_map_entry *md_entry, uint64_t first_lba, uint32_t pg_off)
{
    struct map_cache_entry *cache_ent;
    struct app_map_entry *map_ent;
    uint64_t ent_id;

    if (LIST_EMPTY(&cache->mbf_head))
        if (map_evict_pg_cache (cache))
            return -1;

    cache_ent = LIST_FIRST(&cache->mbf_head);
    if (!cache_ent)
        return -1;

    pthread_spin_lock (&cache->mb_spin);
    LIST_REMOVE(cache_ent, f_entry);
    cache->nfree--;
    pthread_spin_unlock (&cache->mb_spin);

    cache_ent->md_entry = md_entry;

    /* If metadata entry PPA is zero, mapping page does not exist yet */
    if (!md_entry->ppa) {
        for (ent_id = 0; ent_id < ent_per_pg; ent_id++) {
            map_ent = &((struct app_map_entry *) cache_ent->buf)[ent_id];
            map_ent->lba = first_lba + ent_id;
            map_ent->ppa = 0x0;
        }
        cache_ent->dirty = 1;
    } else {
        if (map_nvm_read (cache_ent)) {
            cache_ent->md_entry = NULL;
            cache_ent->ppa.ppa = 0;

            pthread_spin_lock (&cache->mb_spin);
            LIST_INSERT_HEAD(&cache->mbf_head, cache_ent, f_entry);
            cache->nfree++;
            pthread_spin_unlock (&cache->mb_spin);

            return -1;
        }

        /* Cache entry PPA is set after the read completes */
    }

    cache_ent->mutex = &ch[cache->id]->map_md->entry_mutex[pg_off];

    md_entry->ppa = (uint64_t) cache_ent;
    md_entry->ppa |= MAP_ADDR_FLAG;

    pthread_spin_lock (&cache->mb_spin);
    TAILQ_INSERT_TAIL(&cache->mbu_head, cache_ent, u_entry);
    cache->nused++;
    pthread_spin_unlock (&cache->mb_spin);

    return 0;
}

static int map_init_ch_cache (struct map_cache *cache)
{
    uint32_t pg_i;

    cache->pg_buf = calloc (sizeof(struct map_cache_entry) * MAP_BUF_CH_PGS, 1);
    if (!cache->pg_buf)
        return -1;

    if (pthread_spin_init(&cache->mb_spin, 0))
        goto FREE_BUF;

    cache->mbf_head.lh_first = NULL;
    LIST_INIT(&cache->mbf_head);
    TAILQ_INIT(&cache->mbu_head);
    cache->nfree = 0;
    cache->nused = 0;

    for (pg_i = 0; pg_i < MAP_BUF_CH_PGS; pg_i++) {
        cache->pg_buf[pg_i].dirty = 0;
        cache->pg_buf[pg_i].buf_sz = MAP_BUF_PG_SZ;
        cache->pg_buf[pg_i].ppa.ppa = 0x0;
        cache->pg_buf[pg_i].md_entry = NULL;
        cache->pg_buf[pg_i].cache = cache;

        cache->pg_buf[pg_i].buf = malloc (MAP_BUF_PG_SZ);
        if (!cache->pg_buf[pg_i].buf)
            goto FREE_PGS;

        LIST_INSERT_HEAD (&cache->mbf_head, &cache->pg_buf[pg_i], f_entry);
        cache->nfree++;
    }

    return 0;

FREE_PGS:
    while (pg_i) {
        pg_i--;
        LIST_REMOVE(&cache->pg_buf[pg_i], f_entry);
        cache->nfree--;
        free (cache->pg_buf[pg_i].buf);
    }
    pthread_spin_destroy (&cache->mb_spin);
FREE_BUF:
    free (cache->pg_buf);
    return -1;
}

static void map_exit_ch_cache (struct map_cache *cache)
{
    struct map_cache_entry *ent;

    /* Evict all pages in the cache */
    while (!(TAILQ_EMPTY(&cache->mbu_head))) {
        ent = TAILQ_FIRST(&cache->mbu_head);
        if (ent != NULL)
            if (map_evict_pg_cache (cache))
                log_err ("[appnvm (gl_map): ERROR. Cache entry not persisted "
                                                 "in NVM. Ch %d\n", cache->id);
    }

    /* TODO: Check if any cache entry still remains. Retry I/Os */

    while (!(LIST_EMPTY(&cache->mbf_head))) {
        ent = LIST_FIRST(&cache->mbf_head);
        if (ent != NULL) {
            LIST_REMOVE(ent, f_entry);
            cache->nfree--;
            free (ent->buf);
        }
    }

    pthread_spin_destroy (&cache->mb_spin);
    free (cache->pg_buf);
}

static int map_init (void)
{
    uint32_t nch, ch_i, pg_sz;

    ch = malloc (sizeof(struct app_channel *) * app_nch);
    if (!ch)
        return -1;

    nch = appnvm()->channels.get_list_fn (ch, app_nch);
    if (nch != app_nch)
        goto FREE_CH;

    map_ch_cache = malloc (sizeof(struct map_cache) * app_nch);
    if (!map_ch_cache)
        goto FREE_CH;

    pg_sz = ch[0]->ch->geometry->pl_pg_size;
    for (ch_i = 0; ch_i < app_nch; ch_i++) {
        pg_sz = MIN(ch[ch_i]->ch->geometry->pl_pg_size, pg_sz);

        if (map_init_ch_cache (&map_ch_cache[ch_i]))
            goto EXIT_BUF_CH;

        map_ch_cache[ch_i].id = ch_i;
    }

    ent_per_pg = pg_sz / sizeof(struct app_map_entry);

    return 0;

EXIT_BUF_CH:
    while (ch_i) {
        ch_i--;
        map_exit_ch_cache (&map_ch_cache[ch_i]);
    }
    free (map_ch_cache);
FREE_CH:
    free (ch);
    return -1;
}

static void map_exit (void)
{
    uint32_t ch_i = app_nch;

    while (ch_i) {
        ch_i--;
        map_exit_ch_cache (&map_ch_cache[ch_i]);
    }

    free (map_ch_cache);
    free (ch);
}

static struct map_cache_entry *map_get_cache_entry (uint64_t lba)
{
    uint32_t ch_map, pg_off;
    uint64_t first_pg_lba;
    struct app_map_entry *md_ent;
    struct map_cache_entry *cache_ent = NULL;
    struct map_pg_addr *addr;

    /* Mapping metadata pages are spread among channels using round-robin */
    ch_map = (lba / ent_per_pg) % app_nch;
    pg_off = (lba / ent_per_pg) / app_nch;

    md_ent = appnvm()->ch_map.get_fn (ch[ch_map], pg_off);
    if (!md_ent) {
        log_err ("[appnvm (gl_map): Map MD page out of bounds. Ch %d\n",ch_map);
        return NULL;
    }

    addr = (struct map_pg_addr *) &md_ent->ppa;

    /* If the PPA flag is zero, the mapping page is not cached yet */
    /* There is a mutex per metadata page */
    pthread_mutex_lock (&ch[ch_map]->map_md->entry_mutex[pg_off]);
    if (!addr->g.flag) {

        first_pg_lba = (lba / ent_per_pg) * ent_per_pg;

        if (map_load_pg_cache (&map_ch_cache[ch_map], md_ent, first_pg_lba,
                                                                     pg_off)) {
            pthread_mutex_unlock (&ch[ch_map]->map_md->entry_mutex[pg_off]);
            log_err ("[appnvm(gl_map): Mapping page not loaded ch %d\n",ch_map);
            return NULL;
        }

    } else {

        /* Keep cache entry as hot, in the tail of the queue */
        cache_ent = (struct map_cache_entry *) ((uint64_t) addr->g.addr);

        pthread_spin_lock (&map_ch_cache[ch_map].mb_spin);
        TAILQ_REMOVE(&map_ch_cache[ch_map].mbu_head, cache_ent, u_entry);
        TAILQ_INSERT_TAIL(&map_ch_cache[ch_map].mbu_head, cache_ent, u_entry);
        pthread_spin_unlock (&map_ch_cache[ch_map].mb_spin);

    }
    pthread_mutex_unlock (&ch[ch_map]->map_md->entry_mutex[pg_off]);

    /* At this point, the PPA only points to the cache */
    if (cache_ent == NULL)
        cache_ent = (struct map_cache_entry *) ((uint64_t) addr->g.addr);

    return cache_ent;
}

static int map_upsert (uint64_t lba, uint64_t ppa)
{
    uint32_t ch_map, ent_off;
    struct app_map_entry *map_ent;
    struct map_cache_entry *cache_ent;

    ent_off = lba % ent_per_pg;
    if (ent_off >= ent_per_pg) {
        log_err ("[appnvm(gl_map): Entry offset out of bounds. Ch %d\n",ch_map);
        return -1;
    }

    cache_ent = map_get_cache_entry (lba);
    if (!cache_ent)
        return -1;

    map_ent = &((struct app_map_entry *) cache_ent->buf)[ent_off];

    if (map_ent->lba != lba) {
        log_err ("[appnvm(gl_map): LBA does not match entry. lba: %lu, "
                            "map lba: %lu, Ch %d\n", lba, map_ent->lba, ch_map);
        return -1;
    }

    /* Update mapping table entry
       We use no lock here, only 1 thread should update a specific LBA */
    map_ent->ppa = ppa;
    cache_ent->dirty = 1;

    return 0;
}

static uint64_t map_read (uint64_t lba)
{
    struct map_cache_entry *cache_ent;
    struct app_map_entry *map_ent;
    uint32_t ent_off;

    ent_off = lba % ent_per_pg;
    if (ent_off >= ent_per_pg) {
        log_err ("[appnvm(gl_map): Entry offset out of bounds. lba %lu\n", lba);
        return 0;
    }

    cache_ent = map_get_cache_entry (lba);
    if (!cache_ent)
        return 0;

    map_ent = &((struct app_map_entry *) cache_ent->buf)[ent_off];

    return map_ent->ppa;
}

void gl_map_register (void) {
    appnvm()->gl_map.init_fn   = map_init;
    appnvm()->gl_map.exit_fn   = map_exit;
    appnvm()->gl_map.upsert_fn = map_upsert;
    appnvm()->gl_map.read_fn   = map_read;
}