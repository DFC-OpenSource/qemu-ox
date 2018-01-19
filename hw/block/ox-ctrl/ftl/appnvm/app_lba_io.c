/* OX: Open-Channel NVM Express SSD Controller
 *  - AppNVM Flash Translation Layer (Logical Block Address I/O)
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

#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/queue.h>
#include "hw/block/ox-ctrl/include/ssd.h"
#include "appnvm.h"

struct lba_io_sec {
    uint32_t                   lba_id;
    struct nvm_io_cmd         *nvme;
    uint64_t                   lba;
    struct nvm_ppa_addr        ppa;
    uint64_t                   prp;
    uint8_t                    type;
    struct ox_mq_entry        *mentry;
    STAILQ_ENTRY(lba_io_sec)   fentry;
    TAILQ_ENTRY(lba_io_sec)    uentry;
};

struct lba_io_cmd {
    struct nvm_io_cmd            cmd;
    struct lba_io_sec           *vec[64];
    struct app_prov_ppas        *prov;
    STAILQ_ENTRY(lba_io_cmd)     fentry;
    TAILQ_ENTRY(lba_io_cmd)      uentry;
};

struct lba_io_sec_ent {
    uint64_t lba;
    uint64_t ppa;
};

#define LBA_IO_ENTRIES  64
#define LBA_IO_WRITE_Q  0
#define LBA_IO_READ_Q   1
#define LBA_IO_QUEUE_TO 2000000
#define LBA_IO_PPA_SIZE 64

/* PPA I/Os are issued with 64 PPAs. This time in microseconds waits for
 * next command, if time is finished, a smaller PPA I/O command is issued */
#define LBA_IO_EMPTY_US 200

STAILQ_HEAD(flba_q, lba_io_sec) flbahead = STAILQ_HEAD_INITIALIZER(flbahead);
TAILQ_HEAD(ulba_q, lba_io_sec) ulbahead = TAILQ_HEAD_INITIALIZER(ulbahead);
static pthread_spinlock_t sec_spin;

STAILQ_HEAD(fcmd_q, lba_io_cmd) fcmdhead = STAILQ_HEAD_INITIALIZER(fcmdhead);
TAILQ_HEAD(ucmd_q, lba_io_cmd) ucmdhead = TAILQ_HEAD_INITIALIZER(ucmdhead);
static pthread_spinlock_t cmd_spin;

static struct ox_mq        *lba_io_mq;

static uint16_t             sec_pl_pg;
extern uint16_t             app_nch;
static struct app_channel **ch;

/* index 0: write line, index 1: read line */
static struct lba_io_sec   *rw_line[2][64];
static uint8_t              rw_off[2];

static void lba_io_reset_cmd (struct lba_io_cmd *lcmd)
{
    memset (&lcmd->cmd, 0x0, sizeof (struct nvm_io_cmd));
    memset (lcmd->vec, 0x0, sizeof (struct lba_io_sec *) * 64);
    lcmd->prov = 0x0;
}

static void lba_io_callback (struct nvm_io_cmd *cmd)
{
    uint16_t i;
    struct lba_io_cmd *lcmd;
    struct lba_io_sec **lba;
    struct nvm_io_cmd *nvme_cmd;

    lcmd = (struct lba_io_cmd *) cmd;
    lba = lcmd->vec;

    for (i = 0; i < cmd->n_sec; i++) {
        if (!lba[i])
            continue;

        nvme_cmd = lba[i]->nvme;

        /* Check if lba has timeout */
        if (nvme_cmd == 0x0)
            goto COMPLETE_LBA;

        if (nvme_cmd->status.status != NVM_IO_FAIL) {
            nvme_cmd->status.status = cmd->status.status;
            nvme_cmd->status.nvme_status = cmd->status.nvme_status;
        }

COMPLETE_LBA:
        ox_mq_complete_req (lba_io_mq, lba[i]->mentry);
    }

    if ((cmd->cmdtype == MMGR_WRITE_PG) && lcmd->prov)
        appnvm()->gl_prov.free_ppa_list_fn (lcmd->prov);

    lcmd->prov = 0x0;
    pthread_spin_lock (&cmd_spin);
    TAILQ_REMOVE(&ucmdhead, lcmd, uentry);
    STAILQ_INSERT_TAIL(&fcmdhead, lcmd, fentry);
    pthread_spin_unlock (&cmd_spin);
}

static int lba_io_submit (struct nvm_io_cmd *cmd)
{
    uint32_t sec_i, qtype, ret;
    struct lba_io_sec *lba[256];
    qtype = (cmd->cmdtype == MMGR_WRITE_PG) ? LBA_IO_WRITE_Q : LBA_IO_READ_Q;

    for (sec_i = 0; sec_i < cmd->n_sec; sec_i++) {
        if (STAILQ_EMPTY(&flbahead))
            goto REQUEUE;

        pthread_spin_lock (&sec_spin);
        lba[sec_i] = STAILQ_FIRST(&flbahead);
        if (!lba[sec_i]) {
            pthread_spin_unlock (&sec_spin);
            goto REQUEUE;
        }
        STAILQ_REMOVE_HEAD (&flbahead, fentry);
        TAILQ_INSERT_TAIL(&ulbahead, lba[sec_i], uentry);
        pthread_spin_unlock (&sec_spin);

        lba[sec_i]->lba_id = sec_i;
        lba[sec_i]->nvme = cmd;
        lba[sec_i]->lba = cmd->slba + sec_i;
        lba[sec_i]->type = qtype;
        lba[sec_i]->prp = cmd->prp[sec_i];
    }

    for (sec_i = 0; sec_i < cmd->n_sec; sec_i++) {
        if (ox_mq_submit_req(lba_io_mq, qtype, lba[sec_i]))
            /* MQ_TO and callback take care of aborting submitted lbas */
            goto REQUEUE_UNPROCESSED;
    }

    return 0;

REQUEUE_UNPROCESSED:
    cmd->status.status = NVM_IO_FAIL;
    cmd->status.nvme_status = NVME_CAP_EXCEEDED;

    /* If at least 1 lba has been enqueued, let the callback
                                completing the nvme cmd by returning success */
    ret = (sec_i) ? 0 : -1;
    while (sec_i < cmd->n_sec) {
        lba[sec_i]->nvme->status.pgs_p++;
        lba[sec_i]->nvme = 0x0;
        lba[sec_i]->lba = 0x0;
        lba[sec_i]->prp = 0x0;
        pthread_spin_lock (&sec_spin);
        TAILQ_REMOVE(&ulbahead, lba[sec_i], uentry);
        STAILQ_INSERT_TAIL(&flbahead, lba[sec_i], fentry);
        pthread_spin_unlock (&sec_spin);
        sec_i++;
    }
    return ret;

REQUEUE:
    while (sec_i) {
        sec_i--;
        lba[sec_i]->nvme = 0x0;
        lba[sec_i]->lba = 0x0;
        lba[sec_i]->prp = 0x0;
        pthread_spin_lock (&sec_spin);
        TAILQ_REMOVE(&ulbahead, lba[sec_i], uentry);
        STAILQ_INSERT_TAIL(&flbahead, lba[sec_i], fentry);
        pthread_spin_unlock (&sec_spin);
    }
    cmd->status.status = NVM_IO_FAIL;
    cmd->status.nvme_status = NVME_CAP_EXCEEDED;
    return -1;
}

static void lba_io_mq_to (void **opaque, int c)
{
    uint32_t sec_i;
    struct lba_io_sec *lba;

    sec_i = c;
    while (sec_i) {
        sec_i--;
        lba = (struct lba_io_sec *) opaque[sec_i];
        log_err (" [appnvm (lba_io): TIMEOUT LBA-> %lu, cmd-> %p\n",
                                                          lba->lba, lba->nvme);
        lba->nvme->status.status = NVM_IO_FAIL;
        lba->nvme->status.nvme_status = NVME_MEDIA_TIMEOUT;
        lba->nvme->status.pgs_p++;
        lba->nvme = 0x0;
        lba->lba = 0x0;
        lba->prp = 0x0;
        lba->ppa.ppa = 0x0;
    }
}

static void lba_io_prepare_cmd (struct lba_io_cmd *lcmd, uint8_t type)
{
    uint32_t i, pg, nsec;
    struct nvm_io_cmd *cmd = &lcmd->cmd;
    //uint64_t moff;

    cmd->cid = 0; /* Make some counter */
    cmd->sec_sz = NVME_KERNEL_PG_SIZE;
    cmd->md_sz = 0;
    cmd->cmdtype = (!type) ? MMGR_WRITE_PG : MMGR_READ_PG;
    cmd->req = 0x0;

    cmd->status.pg_errors = 0;
    cmd->status.ret_t = 0;
    cmd->status.pgs_p = 0;
    cmd->status.pgs_s = 0;
    cmd->status.status = NVM_IO_PROCESS;

    for (i = 0; i < 8; i++)
        cmd->status.pg_map[i] = 0;

    pg = 0;
    nsec = 0;
    //moff = 0;//meta;
    for (i = 0; i <= cmd->n_sec; i++) {
        if ((i == cmd->n_sec) || (i && (
                      cmd->ppalist[i].g.ch != cmd->ppalist[i - 1].g.ch ||
                      cmd->ppalist[i].g.lun != cmd->ppalist[i - 1].g.lun ||
                      cmd->ppalist[i].g.blk != cmd->ppalist[i - 1].g.blk ||
                      cmd->ppalist[i].g.pg != cmd->ppalist[i - 1].g.pg ||
                      cmd->ppalist[i].g.pl != cmd->ppalist[i - 1].g.pl))) {

            cmd->mmgr_io[pg].pg_index = pg;
            cmd->mmgr_io[pg].status = NVM_IO_SUCCESS;
            cmd->mmgr_io[pg].nvm_io = cmd;
            cmd->mmgr_io[pg].pg_sz = nsec * NVME_KERNEL_PG_SIZE;
            cmd->mmgr_io[pg].n_sectors = nsec;
            cmd->mmgr_io[pg].sec_offset = i - nsec;
            cmd->mmgr_io[pg].sync_count = NULL;
            cmd->mmgr_io[pg].sync_mutex = NULL;
            cmd->md_prp[pg] = 0;//(meta && meta_size) ? moff : 0;

            pg++;
            nsec = 0;
            //moff = 0;//meta + (oob * i);
        }
        nsec++;
    }

    cmd->status.total_pgs = pg;
}

static int lba_io_write (struct lba_io_cmd *lcmd)
{
    struct lba_io_sec_ent *nvme_lba;
    uint32_t sec_i, pgs;
    struct nvm_io_cmd *cmd;
    struct app_prov_ppas *ppas;
    uint32_t nlb = rw_off[LBA_IO_WRITE_Q];

    pgs = nlb / sec_pl_pg;
    if (nlb % sec_pl_pg > 0)
        pgs++;

    ppas = appnvm()->gl_prov.get_ppa_list_fn (pgs);
    if (!ppas || ppas->nppas < nlb)
        return 1;

    lcmd->prov = ppas;
    cmd = &lcmd->cmd;
    cmd->n_sec = sec_pl_pg * pgs;

    for (sec_i = 0; sec_i < nlb; sec_i++) {
        rw_line[LBA_IO_WRITE_Q][sec_i]->ppa.ppa = ppas->ppa[sec_i].ppa;

        cmd->ppalist[sec_i].ppa = ppas->ppa[sec_i].ppa;
        cmd->prp[sec_i]         = rw_line[LBA_IO_WRITE_Q][sec_i]->prp;
        cmd->channel[sec_i]     = ch[ppas->ppa[sec_i].g.ch]->ch;

        lcmd->vec[sec_i]        = rw_line[LBA_IO_WRITE_Q][sec_i];

        /* Keep the LBA/PPAs in the nvme command, in this way, if the command
        fails, no lba is upserted in the mapping table */
        nvme_lba = (struct lba_io_sec_ent *) lcmd->vec[sec_i]->nvme->mmgr_io
                                          [lcmd->vec[sec_i]->lba_id / 4].rsvd;
        nvme_lba = nvme_lba + (lcmd->vec[sec_i]->lba_id % 4);
        nvme_lba->lba = lcmd->vec[sec_i]->lba;
        nvme_lba->ppa = lcmd->vec[sec_i]->ppa.ppa;
    }

    /* Padding the physical write if needed (same data for now) */
    while (sec_i < cmd->n_sec) {
        cmd->ppalist[sec_i].ppa = ppas->ppa[sec_i].ppa;
        cmd->prp[sec_i] = rw_line[LBA_IO_WRITE_Q][0]->prp;
        cmd->channel[sec_i] = ch[ppas->ppa[sec_i].g.ch]->ch;
        sec_i++;
    }

    lba_io_prepare_cmd (lcmd, LBA_IO_WRITE_Q);

    if (appnvm()->ppa_io.submit_fn (cmd)) {
        /* TODO: If PPA IO is failed, tell provisioning to recycle current
         * blocks and abort any write to the blocks, otherwise we loose the
         * sequential writes guarantee within a block. Keep track of status of
         * each LBA and recycle only necessary blocks */
        goto FREE_PPAS;
    }

    return 0;

FREE_PPAS:
    if (lcmd->prov) {
        appnvm()->gl_prov.free_ppa_list_fn (ppas);
        lcmd->prov = 0x0;
    }
    return -1;
}

static int lba_io_read (struct lba_io_cmd *lcmd)
{
    uint32_t sec_i;
    struct nvm_io_cmd *cmd;
    uint32_t nlb = rw_off[LBA_IO_READ_Q];
    struct nvm_ppa_addr sec_ppa;

    cmd = &lcmd->cmd;
    cmd->n_sec = nlb;

    for (sec_i = 0; sec_i < nlb; sec_i++) {

        sec_ppa.ppa = appnvm()->gl_map.read_fn
                                          (rw_line[LBA_IO_READ_Q][sec_i]->lba);
        if (sec_ppa.ppa == AND64)
            return 1;

        rw_line[LBA_IO_READ_Q][sec_i]->ppa.ppa = sec_ppa.ppa;
        cmd->ppalist[sec_i].ppa = sec_ppa.ppa;
        cmd->prp[sec_i] = rw_line[LBA_IO_READ_Q][sec_i]->prp;

        cmd->channel[sec_i] = ch[sec_ppa.g.ch]->ch;

        lcmd->vec[sec_i] = rw_line[LBA_IO_READ_Q][sec_i];
    }

    lba_io_prepare_cmd (lcmd, LBA_IO_READ_Q);

    return appnvm()->ppa_io.submit_fn (cmd);
}

static int lba_io_rw (uint8_t type)
{
    int ret;
    struct lba_io_cmd *lcmd;

    if (STAILQ_EMPTY(&fcmdhead))
        return -1;

    pthread_spin_lock (&cmd_spin);
    lcmd = STAILQ_FIRST(&fcmdhead);
    if (!lcmd) {
        pthread_spin_unlock (&cmd_spin);
        return -1;
    }
    STAILQ_REMOVE_HEAD (&fcmdhead, fentry);
    TAILQ_INSERT_TAIL(&ucmdhead, lcmd, uentry);
    pthread_spin_unlock (&cmd_spin);

    lba_io_reset_cmd (lcmd);

    ret = (!type) ? lba_io_write (lcmd) : lba_io_read (lcmd);

    if (ret)
        goto REQUEUE;

    return 0;

REQUEUE:
    pthread_spin_lock (&cmd_spin);
    TAILQ_REMOVE(&ucmdhead, lcmd, uentry);
    STAILQ_INSERT_TAIL(&fcmdhead, lcmd, fentry);
    pthread_spin_unlock (&cmd_spin);

    return ret;
}

static void lba_io_complete_failed_lbas (uint8_t type)
{
    uint16_t i;
    struct lba_io_sec *lba;

    for (i = 0; i < rw_off[type]; i++) {
        lba = rw_line[type][i];
        lba->nvme->status.status = NVM_IO_FAIL;
        lba->nvme->status.nvme_status = NVME_DATA_TRAS_ERROR;
        ox_mq_complete_req (lba_io_mq, lba->mentry);
    }
}

static void lba_io_sec_sq (struct ox_mq_entry *req)
{
    int ret;
    struct lba_io_sec *lba = (struct lba_io_sec *) req->opaque;
    lba->mentry = req;

    /* 1 write thread and 1 read thread, so, no lock is needed */
    rw_line[lba->type][rw_off[lba->type]] = lba;
    rw_off[lba->type]++;

    ret = ox_mq_used_count (lba_io_mq, lba->type);

    if (ret < 0) {
        lba_io_complete_failed_lbas (lba->type);
        goto RESET_LINE;
    } else if (ret == 0) {
        usleep (LBA_IO_EMPTY_US);
        ret = ox_mq_used_count (lba_io_mq, lba->type);
    }

    if ((rw_off[lba->type] == LBA_IO_PPA_SIZE) || !ret) {
        if (lba_io_rw (lba->type))
            lba_io_complete_failed_lbas (lba->type);

        goto RESET_LINE;
    }

    return;

RESET_LINE:
    rw_off[lba->type] = 0;
    memset (rw_line[lba->type], 0x0, sizeof (struct lba_io_sec *) * 64);
}

static void lba_io_upsert_map (struct nvm_io_cmd *cmd)
{
    uint32_t sec;
    struct lba_io_sec_ent *ent;
    uint64_t old_ppa;

    for (sec = 0; sec < cmd->n_sec; sec++) {
        ent = (struct lba_io_sec_ent *) cmd->mmgr_io[sec / 4].rsvd;
        ent = ent + (sec % 4);

        old_ppa = appnvm()->gl_map.read_fn (ent->lba);
        if (old_ppa == AND64)
            goto ROLLBACK;

        if (appnvm()->gl_map.upsert_fn (ent->lba, ent->ppa))
            goto ROLLBACK;

        ent->ppa = old_ppa;
    }

    return;

ROLLBACK:
    while (sec) {
        sec--;
        ent = (struct lba_io_sec_ent *) cmd->mmgr_io[sec / 4].rsvd;
        ent = ent + (sec % 4);
        if (appnvm()->gl_map.upsert_fn (ent->lba, ent->ppa))
            log_err ("[lba_io: Failed to rollback failed upserted LBAs. "
                                "LBA: %lu, nlbas: %d]", ent->lba - sec, sec);
    }
    cmd->status.status = NVM_IO_FAIL;
    cmd->status.nvme_status = NVME_INTERNAL_DEV_ERROR;
}

static void lba_io_sec_callback (void *opaque)
{
    struct lba_io_sec *lba = (struct lba_io_sec *) opaque;
    struct nvm_io_cmd *nvme_cmd = lba->nvme;

    /* If cmd is NULL, lba has timeout */
    if (nvme_cmd == 0x0)
        goto ERR_TIMEOUT;

    if (nvme_cmd->status.status == NVM_IO_TIMEOUT)
        goto ERR_TIMEOUT;

    pthread_mutex_lock (&nvme_cmd->mutex);
    nvme_cmd->status.pgs_p++;
    pthread_mutex_unlock (&nvme_cmd->mutex);

    if (nvme_cmd->status.pgs_p == nvme_cmd->n_sec) {
        if (lba->type == LBA_IO_WRITE_Q &&
                                     nvme_cmd->status.status == NVM_IO_SUCCESS)
            lba_io_upsert_map (nvme_cmd);

        /* TODO: If nvme cmd is failed, tell provisioning to recycle current
         * block and abort any write to the blocks, otherwise we loose the
         * sequential writes guarantee within a block. Keep track of status of
         * each LBA and recycle only necessary blocks */

        nvm_complete_ftl (nvme_cmd);
    }

ERR_TIMEOUT:
    lba->nvme = 0x0;
    lba->lba = lba->ppa.ppa = lba->prp = 0x0;
    pthread_spin_lock (&sec_spin);
    TAILQ_REMOVE(&ulbahead, lba, uentry);
    STAILQ_INSERT_TAIL(&flbahead, lba, fentry);
    pthread_spin_unlock (&sec_spin);
}

struct ox_mq_config lba_io_mq_config = {
    /* Queue 0: write, queue 1: read */
    .n_queues   = 2,
    .q_size     = LBA_IO_ENTRIES * 128,
    .sq_fn      = lba_io_sec_sq,
    .cq_fn      = lba_io_sec_callback,
    .to_fn      = lba_io_mq_to,
    .to_usec    = LBA_IO_QUEUE_TO,
    .flags      = 0x0
};

static void lba_io_free_cmd (void)
{
    struct lba_io_cmd *cmd;
    struct lba_io_sec *sec;

    while (!STAILQ_EMPTY(&fcmdhead)) {
        cmd = STAILQ_FIRST(&fcmdhead);
        STAILQ_REMOVE_HEAD (&fcmdhead, fentry);
        free (cmd);
    }
    while (!STAILQ_EMPTY(&flbahead)) {
        sec = STAILQ_FIRST(&flbahead);
        STAILQ_REMOVE_HEAD (&flbahead, fentry);
        free (sec);
    }
}

static int lba_io_init (void)
{
    uint32_t cmd_i, lba_i, ch_i, ret;
    struct lba_io_cmd *cmd;
    struct lba_io_sec *sec;

    ch = malloc (sizeof(struct app_channel *) * app_nch);
    if (!ch)
        return -1;

    ret = appnvm()->channels.get_list_fn (ch, app_nch);
    if (ret != app_nch)
        goto FREE_CH;

    sec_pl_pg = ch[0]->ch->geometry->sec_per_pl_pg;
    for (ch_i = 0; ch_i < app_nch; ch_i++)
        sec_pl_pg = MIN(ch[ch_i]->ch->geometry->sec_per_pl_pg, sec_pl_pg);

    STAILQ_INIT(&fcmdhead);
    TAILQ_INIT(&ucmdhead);
    STAILQ_INIT(&flbahead);
    TAILQ_INIT(&ulbahead);

    rw_off[0] = 0;
    rw_off[1] = 0;

    if (pthread_spin_init(&cmd_spin, 0))
        goto FREE_CH;

    if (pthread_spin_init(&sec_spin, 0))
        goto CMD_SPIN;

    for (cmd_i = 0; cmd_i < LBA_IO_ENTRIES; cmd_i++) {
        cmd = calloc (sizeof (struct lba_io_cmd), 1);
        if (!cmd)
            goto FREE_CMD;

        STAILQ_INSERT_TAIL(&fcmdhead, cmd, fentry);

        for (lba_i = 0; lba_i < 64; lba_i++) {
            sec = calloc (sizeof (struct lba_io_sec), 1);
            if (!sec)
                goto FREE_CMD;

            STAILQ_INSERT_TAIL(&flbahead, sec, fentry);
        }
    }

    lba_io_mq = ox_mq_init(&lba_io_mq_config);
    if (!lba_io_mq)
        goto FREE_CMD;

    log_info("    [appnvm: LBA I/O started.]\n");

    return 0;

FREE_CMD:
    lba_io_free_cmd ();
    pthread_spin_destroy (&sec_spin);
CMD_SPIN:
    pthread_spin_destroy (&cmd_spin);
FREE_CH:
    free (ch);
    return -1;
}

static void lba_io_exit (void)
{
    struct lba_io_cmd *cmd;
    struct lba_io_sec *sec;

    ox_mq_destroy(lba_io_mq);

    while (!TAILQ_EMPTY(&ucmdhead)) {
        cmd = TAILQ_FIRST(&ucmdhead);
        TAILQ_REMOVE(&ucmdhead, cmd, uentry);

        STAILQ_INSERT_TAIL(&fcmdhead, cmd, fentry);
    }

    while (!TAILQ_EMPTY(&ulbahead)) {
        sec = TAILQ_FIRST(&ulbahead);
        TAILQ_REMOVE(&ulbahead, sec, uentry);

        ox_mq_complete_req(lba_io_mq, sec->mentry);

        STAILQ_INSERT_TAIL(&flbahead, sec, fentry);
    }

    lba_io_free_cmd ();
    pthread_spin_destroy (&sec_spin);
    pthread_spin_destroy (&cmd_spin);
    free (ch);
}

void lba_io_register (void) {
    appnvm()->lba_io.init_fn     = lba_io_init;
    appnvm()->lba_io.exit_fn     = lba_io_exit;
    appnvm()->lba_io.submit_fn   = lba_io_submit;
    appnvm()->lba_io.callback_fn = lba_io_callback;
}