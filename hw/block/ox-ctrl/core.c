/* OX: OpenChannel NVM Express SSD Controller
 *
 * Copyright (C) 2016, IT University of Copenhagen. All rights reserved.
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 *
 * Funding support provided by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 *
 * This code is licensed under the GNU GPL v2 or later.
 *
 * The controller core is responsible to handle and link:
 *
 *      - Media managers: NAND, DDR, etc.
 *      - Flash Translation Layers + LightNVM raw FTL support
 *      - Interconnect handler: PCIe, network fabric, etc.
 *      - NVMe queues and tail/head doorbells
 *      - NVMe, LightNVM and Fabric(not implemented) command parser
 *      - RDMA handler(not implemented): RoCE, InfiniBand, etc.
 *
 *      Media managers are responsible to manage the flash storage and expose
 *    channels + its geometry (LUNs, blocks, pages, page_size).
 *
 *      Flash Translation Layers are responsible to manage channels, receive
 *    IO commands and send page-granularity IOs to media managers.
 *
 *      Interconnect handlers are responsible to receive NVMe, LightNVM and
 *    Fabric commands from the interconnection (PCIe or Fabrics) and send to
 *    command parser.
 *
 *      NVMe queue support is responsible to read / write to NVMe registers,
 *    process queue doorbells and manage the admin + IO queue(s).
 *
 *      Command parsers are responsible to parse Admin (and execute it) and
 *    IO (send to the FTL) commands coming from the interconnection. It is
 *    possible to set the device as NVMe or LightNVM (OpenChannel device).
 */

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/queue.h>
#include <syslog.h>
#include <mqueue.h>
#include <pthread.h>
#include <signal.h>
#include <setjmp.h>
#include <argp.h>
#include <time.h>
#include "hw/block/ox-ctrl/include/ssd.h"
#include "hw/block/ox-ctrl/include/ox-mq.h"
#include "hw/block/ox-ctrl/include/uatomic.h"
#include "hw/pci/pci.h"

LIST_HEAD(mmgr_list, nvm_mmgr) mmgr_head = LIST_HEAD_INITIALIZER(mmgr_head);
LIST_HEAD(ftl_list, nvm_ftl) ftl_head = LIST_HEAD_INITIALIZER(ftl_head);

struct core_struct core;
struct tests_init_st tests_is __attribute__((weak));

int nvm_register_pcie_handler (struct nvm_pcie *pcie)
{
    if (strlen(pcie->name) > MAX_NAME_SIZE)
        return EMAX_NAME_SIZE;

    if(pthread_create (&pcie->io_thread, NULL, pcie->ops->nvme_consumer, pcie))
        return EPCIE_REGISTER;

    core.nvm_pcie = pcie;

    log_info("  [nvm: PCI Handler registered: %s]\n", pcie->name);

    return 0;
}

static void nvm_calc_print_geo (struct nvm_mmgr_geometry *g)
{
    g->sec_per_pl_pg = g->sec_per_pg * g->n_of_planes;
    g->sec_per_blk = g->sec_per_pl_pg * g->pg_per_blk;
    g->sec_per_lun = g->sec_per_blk * g->blk_per_lun;
    g->sec_per_ch = g->sec_per_lun * g->lun_per_ch;
    g->pg_per_lun = g->pg_per_blk * g->blk_per_lun;
    g->pg_per_ch = g->pg_per_lun * g->lun_per_ch;
    g->blk_per_ch = g->blk_per_lun * g->lun_per_ch;
    g->tot_sec = g->sec_per_ch * g->n_of_ch;
    g->tot_pg = g->pg_per_ch * g->n_of_ch;
    g->tot_blk = g->blk_per_ch * g->n_of_ch;
    g->tot_lun = g->lun_per_ch * g->n_of_ch;
    g->sec_size = g->pg_size / g->sec_per_pg;
    g->pl_pg_size = g->pg_size * g->n_of_planes;
    g->blk_size = g->pl_pg_size * g->pg_per_blk;
    g->lun_size = g->blk_size * g->blk_per_lun;
    g->ch_size = g->lun_size * g->lun_per_ch;
    g->tot_size = g->ch_size * g->n_of_ch;
    g->pg_oob_sz = g->sec_oob_sz * g->sec_per_pg;
    g->pl_pg_oob_sz = g->pg_oob_sz * g->n_of_planes;
    g->blk_oob_sz = g->pl_pg_oob_sz * g->pg_per_blk;
    g->lun_oob_sz = g->blk_oob_sz * g->blk_per_lun;
    g->ch_oob_sz = g->lun_oob_sz * g->lun_per_ch;
    g->tot_oob_sz = g->ch_oob_sz * g->n_of_ch;

    log_info ("   [n_of_planes   = %d]", g->n_of_planes);
    log_info ("   [n_of_channels = %d]", g->n_of_ch);
    log_info ("   [sec_per_pg    = %d]", g->sec_per_pg);
    log_info ("   [sec_per_pl_pg = %d]", g->sec_per_pl_pg);
    log_info ("   [sec_per_blk   = %d]", g->sec_per_blk);
    log_info ("   [sec_per_lun   = %d]", g->sec_per_lun);
    log_info ("   [sec_per_ch    = %d]", g->sec_per_ch);
    log_info ("   [tot_sec       = %lu]", g->tot_sec);
    log_info ("   [pg_per_blk    = %d]", g->pg_per_blk);
    log_info ("   [pg_per_lun    = %d]", g->pg_per_lun);
    log_info ("   [pg_per_ch     = %d]", g->pg_per_ch);
    log_info ("   [tot_pg        = %lu]", g->tot_pg);
    log_info ("   [blk_per_lun   = %d]", g->blk_per_lun);
    log_info ("   [blk_per_ch    = %d]", g->blk_per_ch);
    log_info ("   [tot_blk       = %d]", g->tot_blk);
    log_info ("   [lun_per_ch    = %d]", g->lun_per_ch);
    log_info ("   [tot_lun       = %d]", g->tot_lun);
    log_info ("   [sector_size   = %d bytes, %d KB]",
                                              g->sec_size, g->sec_size / 1024);
    log_info ("   [page_size     = %d bytes, %d KB]",
                                                g->pg_size, g->pg_size / 1024);
    log_info ("   [pl_page_size  = %d bytes, %d KB]",
                                          g->pl_pg_size, g->pl_pg_size / 1024);
    log_info ("   [blk_size      = %d bytes, %d MB]",
                                       g->blk_size, g->blk_size / 1024 / 1024);
    log_info ("   [lun_size      = %lu bytes, %lu MB]",
                                       g->lun_size, g->lun_size / 1024 / 1024);
    log_info ("   [ch_size       = %lu bytes, %lu MB]",
                                         g->ch_size, g->ch_size / 1024 / 1024);
    log_info ("   [tot_size      = %lu bytes, %lu GB]",
                                g->tot_size, g->tot_size / 1024 / 1024 / 1024);
    log_info ("   [sec_oob_sz    = %d bytes]", g->sec_oob_sz);
    log_info ("   [pg_oob_sz     = %d bytes]", g->pg_oob_sz);
    log_info ("   [pl_pg_oob_sz  = %d bytes]", g->pl_pg_oob_sz);
    log_info ("   [blk_oob_sz    = %d bytes, %d KB]",
                                          g->blk_oob_sz, g->blk_oob_sz / 1024);
    log_info ("   [lun_oob_sz    = %d bytes, %d KB]",
                                          g->lun_oob_sz, g->lun_oob_sz / 1024);
    log_info ("   [ch_oob_sz     = %lu bytes, %lu KB]",
                                            g->ch_oob_sz, g->ch_oob_sz / 1024);
    log_info ("   [tot_oob_sz    = %lu bytes, %lu MB]",
                                   g->tot_oob_sz, g->tot_oob_sz / 1024 / 1024);
}

int nvm_register_mmgr (struct nvm_mmgr *mmgr)
{
    struct nvm_mmgr_geometry *g = mmgr->geometry;

    if (strlen(mmgr->name) > MAX_NAME_SIZE)
        return EMAX_NAME_SIZE;

    mmgr->ch_info = calloc(sizeof(struct nvm_channel), g->n_of_ch);
    if (!mmgr->ch_info)
        return EMMGR_REGISTER;

    if (mmgr->ops->get_ch_info(mmgr->ch_info, g->n_of_ch))
        return EMMGR_REGISTER;

    LIST_INSERT_HEAD(&mmgr_head, mmgr, entry);
    core.mmgr_count++;
    core.nvm_ch_count += g->n_of_ch;

    log_info("  [nvm: Media Manager registered: %s]]", mmgr->name);

    nvm_calc_print_geo (g);

    return 0;
}

static struct nvm_ftl *nvm_get_ftl_instance(uint16_t ftl_id)
{
    struct nvm_ftl *ftl;
    LIST_FOREACH(ftl, &ftl_head, entry){
        if(ftl->ftl_id == ftl_id)
            return ftl;
    }

    return NULL;
}

static uint16_t nvm_ftl_q_schedule (struct nvm_ftl *ftl,
                                      struct nvm_io_cmd *cmd, uint8_t multi_ch)
{
    uint16_t qid;

    /* Separate writes and reads in different queues for AppNVM FTL */
    if (ftl->ftl_id == FTL_ID_APPNVM) {
        qid = (cmd->cmdtype == MMGR_WRITE_PG) ? 0 : 1;

        ftl->next_queue[qid] = (ftl->next_queue[qid] + 1 == ftl->nq / 2) ?
                                                  0 : ftl->next_queue[qid] + 1;

        return ftl->next_queue[qid] + (qid * (ftl->nq / 2));
    }

    if (!multi_ch)
        return cmd->channel[0]->ch_id % ftl->nq;
    else {
        qid = ftl->next_queue[0];
        ftl->next_queue[0] = (ftl->next_queue[0] + 1 == ftl->nq) ?
                                                    0 : ftl->next_queue[0] + 1;
        return qid;
    }
}

static void nvm_complete_to_host (struct nvm_io_cmd *cmd)
{
    NvmeRequest *req = (NvmeRequest *) cmd->req;

    req->status = (cmd->status.status == NVM_IO_SUCCESS) ?
                NVME_SUCCESS : (cmd->status.nvme_status) ?
                      cmd->status.nvme_status : NVME_CMD_ABORT_REQ;

    if (core.debug)
        printf(" [NVMe cmd 0x%x. cid: %d completed. Status: %x]\n",
                                   req->cmd.opcode, req->cmd.cid, req->status);

    ((core.run_flag & RUN_TESTS) && core.tests_init->complete_io) ?
        core.tests_init->complete_io(req) : nvme_rw_cb(req);
}

void nvm_complete_ftl (struct nvm_io_cmd *cmd)
{
    int retry, ret;
    struct ox_mq_entry *req = (struct ox_mq_entry *) cmd->mq_req;

    retry = NVM_QUEUE_RETRY;
    do {
        ret = ox_mq_complete_req(cmd->channel[0]->ftl->mq, req);
        if (ret) {
            retry--;
            usleep (NVM_QUEUE_RETRY_SLEEP);
        }
    } while (ret && retry);
}

static void nvm_ftl_process_sq (struct ox_mq_entry *req)
{
    struct nvm_io_cmd *cmd = (struct nvm_io_cmd *) req->opaque;
    struct nvm_ftl *ftl = cmd->channel[0]->ftl;
    int ret, retry;

    cmd->mq_req = (void *) req;

    retry = NVM_QUEUE_RETRY;
    do {
        ret = ftl->ops->submit_io(cmd);
        if (ret) {
            retry--;
            usleep (NVM_QUEUE_RETRY_SLEEP);
            if (retry) {
                cmd->status.nvme_status = NVME_SUCCESS;
                cmd->status.status = NVM_IO_PROCESS;
            }
        }
    } while (ret && retry);

    if (ret) {
        if (core.debug)
            log_err ("[ftl: Cmd %lu (0x%x) NOT completed. ret %d]\n",
                                                   cmd->cid,cmd->cmdtype, ret);
        nvm_complete_ftl(cmd);
    }
}

static void nvm_ftl_process_cq (void *opaque)
{
    struct nvm_io_cmd *cmd = (struct nvm_io_cmd *) opaque;

    nvm_complete_to_host (cmd);
}

static void nvm_ftl_process_to (void **opaque, int counter)
{
    struct nvm_io_cmd *cmd;

    while (counter) {
        counter--;
        cmd = (struct nvm_io_cmd *) opaque[counter];
        cmd->status.status = NVM_IO_TIMEOUT;
        cmd->status.nvme_status = NVME_MEDIA_TIMEOUT;
    }
}

int nvm_register_ftl (struct nvm_ftl *ftl)
{
    struct ox_mq_config mq_config;

    if (strlen(ftl->name) > MAX_NAME_SIZE)
        return EMAX_NAME_SIZE;

    /* Start FTL multi-queue */
    sprintf(mq_config.name, "%s", ftl->name);
    mq_config.n_queues = ftl->nq;
    mq_config.q_size = NVM_FTL_QUEUE_SIZE;
    mq_config.sq_fn = nvm_ftl_process_sq;
    mq_config.cq_fn = nvm_ftl_process_cq;
    mq_config.to_fn = nvm_ftl_process_to;
    mq_config.to_usec = NVM_FTL_QUEUE_TO;
    mq_config.flags = OX_MQ_TO_COMPLETE;
    ftl->mq = ox_mq_init(&mq_config);
    if (!ftl->mq)
        return -1;

    ftl->next_queue[0] = 0;
    ftl->next_queue[1] = 0;

    core.ftl_q_count += ftl->nq;

    LIST_INSERT_HEAD(&ftl_head, ftl, entry);
    core.ftl_count++;

    log_info("  [nvm: FTL (%s)(%d) registered.]\n", ftl->name, ftl->ftl_id);
    if (ftl->cap & 1 << FTL_CAP_GET_BBTBL)
        log_info("    [%s cap: Get Bad Block Table]\n", ftl->name);
    if (ftl->cap & 1 << FTL_CAP_SET_BBTBL)
        log_info("    [%s cap: Set Bad Block Table]\n", ftl->name);
    if (ftl->cap & 1 << FTL_CAP_GET_L2PTBL)
        log_info("    [%s cap: Get Logical to Physical Table]\n", ftl->name);
    if (ftl->cap & 1 << FTL_CAP_GET_L2PTBL)
        log_info("    [%s cap: Set Logical to Physical Table]\n", ftl->name);
    if (ftl->cap & 1 << FTL_CAP_INIT_FN)
        log_info("    [%s cap: Application Function Init]\n", ftl->name);
    if (ftl->cap & 1 << FTL_CAP_EXIT_FN)
        log_info("    [%s cap: Application Function Exit]\n", ftl->name);

    if (ftl->bbtbl_format == FTL_BBTBL_BYTE)
        log_info("    [%s Bad block table type: Byte array. 1 byte per blk.]\n",
                                                                    ftl->name);
    return 0;
}

static void nvm_debug_print_mmgr_io (struct nvm_mmgr_io_cmd *cmd)
{
    printf (" [IO CALLBK. CMD 0x%x. mmgr_ch: %d, lun: %d, blk: %d, pl: %d, "
                     "pg: %d]\n", cmd->cmdtype, cmd->ppa.g.ch, cmd->ppa.g.lun,
                      cmd->ppa.g.blk, cmd->ppa.g.pl, cmd->ppa.g.pg);
}

void nvm_callback (struct nvm_mmgr_io_cmd *cmd)
{
    if (nvm_memcheck(cmd))
        return;

    gettimeofday(&cmd->tend,NULL);

    if (core.debug)
        nvm_debug_print_mmgr_io (cmd);

    if (cmd->status != NVM_IO_SUCCESS)
        log_err (" [FAILED 0x%x CMD. mmgr_ch: %d, lun: %d, blk: %d, pl: %d, "
                "pg: %d]\n", cmd->cmdtype, cmd->ppa.g.ch, cmd->ppa.g.lun,
                cmd->ppa.g.blk, cmd->ppa.g.pl, cmd->ppa.g.pg);

    if (cmd->sync_count) {
        pthread_mutex_lock(cmd->sync_mutex);
        if (cmd->status != NVM_IO_TIMEOUT) {
            u_atomic_dec(cmd->sync_count);
        }
        pthread_mutex_unlock(cmd->sync_mutex);
    } else {
        cmd->ch->ftl->ops->callback_io(cmd);
    }
}

int nvm_submit_ftl (struct nvm_io_cmd *cmd)
{
    struct nvm_ftl *ftl;
    int ret, retry, qid, i;
    uint8_t ch_ppa[core.nvm_ch_count];

    uint8_t multi_ch = 0;
    NvmeRequest *req = (NvmeRequest *) cmd->req;

    cmd->status.nvme_status = NVME_SUCCESS;

    switch (cmd->status.status) {
        case NVM_IO_NEW:
            break;
        case NVM_IO_PROCESS:
            break;
        case NVM_IO_FAIL:
            req->status = NVME_CMD_ABORT_REQ;
            nvm_complete_to_host(cmd);
            return NVME_CMD_ABORT_REQ;
        case NVM_IO_SUCCESS:
            req->status = NVME_SUCCESS;
            nvm_complete_to_host(cmd);
            return NVME_SUCCESS;
        default:
            req->status = NVME_CMD_ABORT_REQ;
            nvm_complete_to_host(cmd);
            return NVME_CMD_ABORT_REQ;
    }

    if (core.lnvm) {

        /*For now, the host ppa channel must be aligned with core.nvm_ch[] */
        /*All PPAs in the vector must address channels managed by the same FTL*/
        if (cmd->ppalist[0].g.ch >= core.nvm_ch_count)
            goto CH_ERR;

        ftl = core.nvm_ch[cmd->ppalist[0].g.ch]->ftl;

        memset (ch_ppa, 0, sizeof (uint8_t) * core.nvm_ch_count);

        /* Per PPA checking */
        for (i = 0; i < cmd->n_sec; i++) {
            if (cmd->ppalist[i].g.ch >= core.nvm_ch_count)
                goto CH_ERR;

            ch_ppa[cmd->ppalist[i].g.ch]++;

            if (!multi_ch && cmd->ppalist[i].g.ch != cmd->ppalist[0].g.ch)
                multi_ch++;

            if (core.nvm_ch[cmd->ppalist[i].g.ch]->ftl != ftl)
                goto FTL_ERR;

            /* Set channel per PPA */
            cmd->channel[i] = core.nvm_ch[cmd->ppalist[i].g.ch];
        }

    } else {

        /* For now all channels must be included in the global namespace */
        if ((cmd->slba > (core.nvm_ns_size / cmd->sec_sz)) ||
                (cmd->slba + (cmd->sec_sz * cmd->n_sec) > core.nvm_ns_size)) {
            syslog(LOG_INFO,"[nvm: IO out of bounds.]\n");
            req->status = NVME_LBA_RANGE;
            nvm_complete_to_host(cmd);
            return NVME_LBA_RANGE;
        }

        cmd->channel[0] = core.nvm_ch[cmd->slba /
                                       (core.nvm_ns_size / core.nvm_ch_count)];
        multi_ch++;

        ftl = nvm_get_ftl_instance(core.std_ftl);

    }

    cmd->status.status = NVM_IO_PROCESS;
    retry = NVM_QUEUE_RETRY;

    qid = nvm_ftl_q_schedule (ftl, cmd, multi_ch);
    do {
        ret = ox_mq_submit_req(ftl->mq, qid, cmd);

        if (ret) {
            retry--;
            usleep (NVM_QUEUE_RETRY_SLEEP);
        }
        else if (core.debug) {
            printf(" CMD cid: %lu, type: 0x%x submitted to FTL. "
                               "FTL queue: %d\n", cmd->cid, cmd->cmdtype, qid);
            if (core.lnvm) {
                for (i = 0; i < core.nvm_ch_count; i++)
                    if (ch_ppa[i] > 0)
                        printf("  Channel: %d, PPAs: %d\n", i, ch_ppa[i]);
            }
        }
    } while (ret && retry);

    return (retry) ? NVME_NO_COMPLETE : NVME_CMD_ABORT_REQ;

CH_ERR:
    syslog(LOG_INFO,"[nvm ERROR: IO failed, channel not found.]\n");
    req->status = NVME_CMD_ABORT_REQ;
    nvm_complete_to_host(cmd);
    return NVME_CMD_ABORT_REQ;

FTL_ERR:
    syslog(LOG_INFO,"[nvm ERROR: IO failed, channels do not match FTL.]\n");
    req->status = NVME_INVALID_FIELD;
    nvm_complete_to_host(cmd);
    return NVME_INVALID_FIELD;
}

int nvm_dma (void *ptr, uint64_t prp, ssize_t size, uint8_t direction)
{
    if (!size || !prp)
        return 0;

    switch (direction) {
        case NVM_DMA_TO_HOST:
            return nvme_write_to_host(ptr, prp, size);
        case NVM_DMA_FROM_HOST:
            return nvme_read_from_host(ptr, prp, size);
        case NVM_DMA_SYNC_READ:
            memcpy ((void *) prp, ptr, size);
            break;
        case NVM_DMA_SYNC_WRITE:
            memcpy (ptr, (void *) prp, size);
            break;
        default:
            return -1;
    }
    return 0;
}

int nvm_submit_mmgr (struct nvm_mmgr_io_cmd *cmd)
{
    gettimeofday(&cmd->tstart,NULL);
    cmd->cmdtype = cmd->nvm_io->cmdtype;

    switch (cmd->nvm_io->cmdtype) {
        case MMGR_WRITE_PG:
            return cmd->ch->mmgr->ops->write_pg(cmd);
        case MMGR_READ_PG:
            return cmd->ch->mmgr->ops->read_pg(cmd);
        case MMGR_ERASE_BLK:
            return cmd->ch->mmgr->ops->erase_blk(cmd);
        default:
            return -1;
    }
}

static void nvm_sync_io_free (uint8_t flags, void *buf,
                                                   struct nvm_mmgr_io_cmd *cmd)
{
    if (flags & NVM_SYNCIO_FLAG_DEC) {
        pthread_mutex_lock(cmd->sync_mutex);
        u_atomic_dec(cmd->sync_count);
        pthread_mutex_unlock(cmd->sync_mutex);
    }

    if (flags & NVM_SYNCIO_FLAG_BUF)
        free (buf);

    if (flags & NVM_SYNCIO_FLAG_SYNC) {
        pthread_mutex_destroy (cmd->sync_mutex);
        free (cmd->sync_count);
        free (cmd->sync_mutex);
    }
}

static int nvm_sync_io_prepare (struct nvm_channel *ch,
                        struct nvm_mmgr_io_cmd *cmd, void *buf, uint8_t flags)
{
    int i;

    if (!ch)
        return -1;

    if (!cmd->sync_count || !cmd->sync_mutex) {
        cmd->sync_count = malloc (sizeof (u_atomic_t));
        if (!cmd->sync_count)
            return -1;
        cmd->sync_mutex = malloc (sizeof (pthread_mutex_t));
        if (!cmd->sync_mutex) {
            free (cmd->sync_count);
            return -1;
        }
        cmd->sync_count->counter = U_ATOMIC_INIT_RUNTIME(0);
        pthread_mutex_init (cmd->sync_mutex, NULL);
        flags |= NVM_SYNCIO_FLAG_SYNC;
    }

    cmd->ppa.g.ch = ch->ch_mmgr_id;

    if (cmd->cmdtype == MMGR_ERASE_BLK)
        goto OUT;

    if (cmd->pg_sz == 0)
        cmd->pg_sz = NVM_PG_SIZE;
    if (cmd->sec_sz == 0)
        cmd->sec_sz = ch->geometry->pg_size / ch->geometry->sec_per_pg;
    if (cmd->md_sz == 0)
        cmd->md_sz = ch->geometry->sec_oob_sz * ch->geometry->sec_per_pg;
    if (cmd->n_sectors == 0)
        cmd->n_sectors = ch->geometry->sec_per_pg;

    if (!buf) {
        buf = malloc(ch->geometry->pg_size + ch->geometry->sec_oob_sz);
        if (!buf) {
            nvm_sync_io_free (flags, NULL, cmd);
            return -1;
        }
        flags |= NVM_SYNCIO_FLAG_BUF;
    }

    if (cmd->cmdtype == MMGR_READ_SGL || cmd->cmdtype == MMGR_WRITE_SGL) {

        for (i = 0; i < cmd->n_sectors; i++)
            cmd->prp[i] = (uint64_t) ((uint8_t **) buf)[i];
        cmd->md_prp = (uint64_t) ((uint8_t **) buf)[cmd->n_sectors];
        cmd->cmdtype = (cmd->cmdtype == MMGR_READ_SGL) ?
                                                  MMGR_READ_PG : MMGR_WRITE_PG;
    } else {

        for (i = 0; i < cmd->n_sectors; i++)
            cmd->prp[i] = (uint64_t) buf + cmd->sec_sz * i;
        cmd->md_prp = (uint64_t) buf + cmd->sec_sz * cmd->n_sectors;

    }

OUT:
    return 0;
}

/**
 * Submit an IO to a specific channel and wait for completion to return.
 * Please, use nvm_submit_multi_plane_sync_io if you have planes > 1
 *
 * BE CAREFUL WHEN MULTIPLE THREADS USE THE SAME ATOMIC INT: If cmd->sync_count
 * and cmd->sync_mutex are not NULL, multiple threads can share the same
 * waiting loop. If so, all threads will return when all IOs are completed.
 *
 * @param ch - nvm_channel pointer
 * @param cmd - Media manager command pointer
 * @param buf - Pointer to dma data
 * @param cmdtype - MMGR_READ_PG, MMGR_WRITE_PG, MMGR_ERASE_BLK
 * @return 0 on success
 */
int nvm_submit_sync_io (struct nvm_channel *ch, struct nvm_mmgr_io_cmd *cmd,
                                                    void *buf, uint8_t cmdtype)
{
    int ret = 0, i;
    uint8_t flags = 0;
    time_t start;
    struct nvm_mmgr *mmgr;
    char err[64];

    cmd->cmdtype = cmdtype;
    if (nvm_sync_io_prepare (ch, cmd, buf, flags))
        goto ERR;

    mmgr = ch->mmgr;
    if (!mmgr)
        goto ERR;

    pthread_mutex_lock(cmd->sync_mutex);
    u_atomic_inc(cmd->sync_count);
    pthread_mutex_unlock(cmd->sync_mutex);
    flags |= NVM_SYNCIO_FLAG_DEC;

    cmd->status = NVM_IO_PROCESS;

    gettimeofday(&cmd->tstart,NULL);

    switch (cmd->cmdtype) {
        case MMGR_READ_PG:
            ret = mmgr->ops->read_pg(cmd);
            break;
        case MMGR_WRITE_PG:
            ret = mmgr->ops->write_pg(cmd);
            break;
        case MMGR_ERASE_BLK:
            ret = mmgr->ops->erase_blk(cmd);
            break;
        default:
            ret = -1;
    }

    if (core.debug)
        printf("[Sync IO: 0x%x ppa: ch %d, lun %d, blk %d, pl %d, pg %d]\n",
                cmd->cmdtype, cmd->ppa.g.ch, cmd->ppa.g.lun, cmd->ppa.g.blk,
                cmd->ppa.g.pl, cmd->ppa.g.pg);

    if (ret) {
        nvm_sync_io_free (flags, buf, cmd);
        goto ERR;
    }

    /* Concurrent threads with the same sync_counter wait others to complete */
    start = time(NULL);
    i = 1;
    do {
        if (time(NULL) > start + NVM_SYNCIO_TO) {
            cmd->status = NVM_IO_TIMEOUT;
            nvm_sync_io_free (flags, buf, cmd);
            log_err ("[nvm: Sync IO cmd 0x%x TIMEOUT. Aborted.]\n",cmd->cmdtype);
            return -1;
        }
        pthread_mutex_lock(cmd->sync_mutex);
        i = u_atomic_read(cmd->sync_count);
        pthread_mutex_unlock(cmd->sync_mutex);
        if (i)
            usleep(1);
    } while (i || cmd->status == NVM_IO_PROCESS);

    ret = (cmd->status == NVM_IO_SUCCESS) ? 0 : -1;

    flags ^= NVM_SYNCIO_FLAG_DEC;
    nvm_sync_io_free (flags, buf, cmd);

    if (!ret) return ret;
ERR:
    sprintf(err, "[ERROR: Sync IO cmd 0x%x with errors. Aborted.]\n",
                                                                  cmd->cmdtype);
    log_err ("%s",err);
    if (core.debug) printf ("%s",err);

    return -1;
}

static void *nvm_sub_pl_sio_th (void *arg)
{
    struct nvm_sync_io_arg *ptr = (struct nvm_sync_io_arg *) arg;
    ptr->status = nvm_submit_sync_io(ptr->ch, ptr->cmd, ptr->buf, ptr->cmdtype);

    return NULL;
}

/**
 * Use this function to submit a synchronous IO using multi-plane. All pages
 * within the plane-page will be asynchronous. Refer to nvm_submit_sync_io
 * for further info about synchronous IO.
 *
 * ENSURE YOUR *buf HAS ENOUGH SPACE FOR N_PLANES * (PAGE_SIZE + OOB) and *cmd
 * is an array of N_PLANES nvm_mmgr_io_cmd
 *
 * @params Refer to nvm_submit_sync_io
 * @pl_delay Delay between plane-page IOs in u-seconds, 0 to ignore the delay
 */

int nvm_submit_multi_plane_sync_io (struct nvm_channel *ch,
     struct nvm_mmgr_io_cmd *cmd, void *buf, uint8_t cmdtype, uint64_t pl_delay)
{
    int pl_i, ret = 0, pl = ch->geometry->n_of_planes;
    pthread_t               tid[pl];
    struct nvm_sync_io_arg  pl_arg[pl];

    for (pl_i = 0; pl_i < pl; pl_i++) {
        cmd[pl_i].ppa.g.pl = pl_i;
        if (cmdtype != MMGR_ERASE_BLK)
            pl_arg[pl_i].buf = buf + (NVM_PG_SIZE + NVM_OOB_SIZE) * pl_i;

        pl_arg[pl_i].ch = ch;
        pl_arg[pl_i].cmdtype = cmdtype;
        pl_arg[pl_i].cmd = &cmd[pl_i];
        pthread_create(&tid[pl_i],NULL,nvm_sub_pl_sio_th,(void *)&pl_arg[pl_i]);

        if (pl_delay > 0)
            usleep(pl_delay);
    }

    for (pl_i = 0; pl_i < pl; pl_i++) {
        pthread_join(tid[pl_i], NULL);
        if (pl_arg->status) ret++;
    }

    return ret;
}

static void nvm_unregister_mmgr (struct nvm_mmgr *mmgr)
{
    if (LIST_EMPTY(&mmgr_head))
        return;

    mmgr->ops->exit(mmgr);
    free(mmgr->ch_info);
    LIST_REMOVE(mmgr, entry);
    core.mmgr_count--;
    core.nvm_ch_count -= mmgr->geometry->n_of_ch;
    log_info(" [nvm: Media Manager unregistered: %s]\n", mmgr->name);
}

static void nvm_unregister_ftl (struct nvm_ftl *ftl)
{
    if (LIST_EMPTY(&ftl_head))
        return;

    ox_mq_destroy(ftl->mq);
    core.ftl_q_count -= ftl->nq;
    ftl->ops->exit();
    LIST_REMOVE(ftl, entry);
    core.ftl_count--;
    log_info(" [nvm: FTL (%s)(%d) unregistered.]\n", ftl->name, ftl->ftl_id);
}

static int nvm_ch_config (void)
{
    int i, c = 0, ret;

    core.nvm_ch = calloc(sizeof(struct nvm_channel *), core.nvm_ch_count);
    if (nvm_memcheck(core.nvm_ch))
        return EMEM;

    core.nvm_ns_size = 0;

    struct nvm_channel *ch;
    struct nvm_mmgr *mmgr;
    LIST_FOREACH(mmgr, &mmgr_head, entry){
        for (i = 0; i < mmgr->geometry->n_of_ch; i++){
            ch = core.nvm_ch[c] = &mmgr->ch_info[i];
            ch->ch_id       = c;
            ch->geometry    = mmgr->geometry;
            ch->mmgr        = mmgr;

            /* For now we set all channels to be managed by the standard FTL */
            /* For now all channels are set to the same namespace */
            if (ch->i.in_use != NVM_CH_IN_USE) {
                ch->i.in_use = NVM_CH_IN_USE;
                ch->i.ns_id = 0x1;
                ch->i.ns_part = c;
                ch->i.ftl_id = core.std_ftl;

                /* FLush new config to NVM */
                mmgr->ops->set_ch_info(ch, 1);
            }

            ch->ftl = nvm_get_ftl_instance(ch->i.ftl_id);

            if(!ch->ftl || !ch->mmgr || ch->geometry->pg_size != NVM_PG_SIZE)
                return ECH_CONFIG;

            /* ftl must set available number of pages */
            ret = ch->ftl->ops->init_ch(ch);
                if (ret) return ret;

            ch->tot_bytes = ch->ns_pgs *
                                  (ch->geometry->pg_size & 0xffffffffffffffff);
            ch->slba = core.nvm_ns_size;
            ch->elba = core.nvm_ns_size + ch->tot_bytes - 1;

            core.nvm_ns_size += ch->tot_bytes;
            c++;
        }
    }

#if FTL_APPNVM
    /* APPNVM Global Init */
    struct nvm_ftl_cap_gl_fn app_gl;
    app_gl.arg = NULL;
    app_gl.ftl_id = FTL_ID_APPNVM;
    app_gl.fn_id = 0;
    if (nvm_ftl_cap_exec(FTL_CAP_INIT_FN, &app_gl))
        return -1;
    core.run_flag |= RUN_APPNVM;
#endif
    return 0;
}

static int nvm_ftl_cap_get_bbtbl (struct nvm_channel *ch,
                                          struct nvm_ftl_cap_get_bbtbl_st *arg)
{
    if (arg->nblk < 1)
        return -1;

    if (!ch->ftl->ops->get_bbtbl)
        return -1;

    if (ch->ftl->bbtbl_format == arg->bb_format)
        return ch->ftl->ops->get_bbtbl(&arg->ppa, arg->bbtbl, arg->nblk);

    return -1;
}

static int nvm_ftl_cap_set_bbtbl (struct nvm_channel *ch,
                                          struct nvm_ftl_cap_set_bbtbl_st *arg)
{
    if (!ch->ftl->ops->set_bbtbl)
        return -1;

    if (ch->ftl->bbtbl_format == arg->bb_format)
        return ch->ftl->ops->set_bbtbl(&arg->ppa, arg->value);

    return -1;
}

static int nvm_ftl_cap_init_fn (struct nvm_ftl *ftl,
                                                 struct nvm_ftl_cap_gl_fn *arg)
{
    if (!ftl->ops->init_fn)
        return -1;

    return ftl->ops->init_fn (arg->fn_id, arg->arg);
}

static int nvm_ftl_cap_exit_fn (struct nvm_ftl *ftl,
                                                 struct nvm_ftl_cap_gl_fn *arg)
{
    if (!ftl->ops->exit_fn)
        return -1;

    ftl->ops->exit_fn (arg->fn_id);

    return 0;
}

int nvm_ftl_cap_exec (uint8_t cap, void *arg)
{
    struct nvm_channel *ch;
    struct nvm_ftl_cap_set_bbtbl_st *set_bbtbl;
    struct nvm_ftl_cap_get_bbtbl_st *get_bbtbl;
    struct nvm_ftl_cap_gl_fn        *gl_fn;
    struct nvm_ftl                  *ftl;

    if (!arg)
        return -1;

    switch (cap) {
        case FTL_CAP_GET_BBTBL:

            get_bbtbl = (struct nvm_ftl_cap_get_bbtbl_st *) arg;
            if (get_bbtbl->ppa.g.ch >= core.nvm_ch_count)
                goto OUT;
            ch = core.nvm_ch[get_bbtbl->ppa.g.ch];
            if (ch->ftl->cap & 1 << FTL_CAP_GET_BBTBL) {
                if (nvm_ftl_cap_get_bbtbl(ch, arg))
                    goto OUT;
                return 0;
            }
            break;

        case FTL_CAP_SET_BBTBL:

            set_bbtbl = (struct nvm_ftl_cap_set_bbtbl_st *) arg;
            if (set_bbtbl->ppa.g.ch >= core.nvm_ch_count)
                goto OUT;
            ch = core.nvm_ch[set_bbtbl->ppa.g.ch];
            if (ch->ftl->cap & 1 << FTL_CAP_SET_BBTBL) {
                if (nvm_ftl_cap_set_bbtbl(ch, arg))
                    goto OUT;
                return 0;
            }
            break;

        case FTL_CAP_GET_L2PTBL:
        case FTL_CAP_SET_L2PTBL:
        case FTL_CAP_INIT_FN:

            gl_fn = (struct nvm_ftl_cap_gl_fn *) arg;
            ftl = nvm_get_ftl_instance(gl_fn->ftl_id);
            if (!ftl)
                goto OUT;
            if (ftl->cap & 1 << FTL_CAP_INIT_FN) {
                if (nvm_ftl_cap_init_fn(ftl, gl_fn))
                    goto OUT;
                return 0;
            }
            break;

        case FTL_CAP_EXIT_FN:

            gl_fn = (struct nvm_ftl_cap_gl_fn *) arg;
            ftl = nvm_get_ftl_instance(gl_fn->ftl_id);
            if (!ftl)
                goto OUT;
            if (ftl->cap & 1 << FTL_CAP_EXIT_FN) {
                if (nvm_ftl_cap_exit_fn(ftl, gl_fn))
                    goto OUT;
                return 0;
            }
            break;

        default:
            goto OUT;
    }

OUT:
    log_err ("[ERROR: FTL capability: %x. Aborted.]\n", cap);
    return -1;
}

static int nvm_init (uint8_t start_all)
{
    int ret;

    core.ftl_q_count = 0;
    core.run_flag = 0;
    core.ftl_count = 0;

    if (start_all) {
        core.mmgr_count = 0;
        core.nvm_ch_count = 0;
        core.nvm_nvme_ctrl = malloc (sizeof (NvmeCtrl));
    }

    if (!core.nvm_nvme_ctrl)
        return EMEM;
    core.nvm_nvme_ctrl->running = 1; /* not ready */
    core.run_flag |= RUN_NVME_ALLOC;

    /* media managers */
#if MMGR_DFCNAND
    ret = mmgr_dfcnand_init();
    if(ret) goto OUT;
#endif
#if MMGR_VOLT
    ret = mmgr_volt_init();
    if(ret) goto OUT;
#endif
    core.run_flag |= RUN_MMGR;

    /* flash translation layers */
#if FTL_LNVMFTL
    ret = ftl_lnvm_init();
    if(ret) goto OUT;
#endif
#if FTL_APPNVM
    ret = ftl_appnvm_init();
    if(ret) goto OUT;
#endif
    core.run_flag |= RUN_FTL;

    /* create channels and global namespace */
    ret = nvm_ch_config();
    if(ret) goto OUT;
    core.run_flag |= RUN_CH;

    /* pci handler */
    if (start_all) {
        ret = dfcpcie_init();
        if(ret) goto OUT;
        core.run_flag |= RUN_PCIE;
    }

    /* nvme standard */
    ret = nvme_init(core.nvm_nvme_ctrl);
    if(ret) goto OUT;
    core.run_flag |= RUN_NVME;

    core.nvm_nvme_ctrl->running = 0; /* ready */
    core.nvm_nvme_ctrl->stop = 0;

    printf("OX Controller started succesfully. Log: /var/log/syslog\n");

    return 0;

OUT:
    return ret;
}

static void nvm_clear_all (uint8_t stop_all)
{
    struct nvm_ftl *ftl;
    struct nvm_mmgr *mmgr;

    /* Clean PCIe handler */
    if(core.nvm_pcie && (core.run_flag & RUN_PCIE) && stop_all) {
        core.nvm_pcie->ops->exit();
        core.run_flag ^= RUN_PCIE;
    }

#if FTL_APPNVM
    /* APPNVM Global Exit */
    struct nvm_ftl_cap_gl_fn app_gl;
    if (core.run_flag & RUN_APPNVM) {
        app_gl.arg = NULL;
        app_gl.ftl_id = FTL_ID_APPNVM;
        app_gl.fn_id = 0;
        nvm_ftl_cap_exec(FTL_CAP_EXIT_FN, &app_gl);
        core.run_flag ^= RUN_APPNVM;
    }
#endif

    /* Clean channels */
    if (core.run_flag & RUN_CH) {
        free(core.nvm_ch);
        core.run_flag ^= RUN_CH;
    }

    /* Clean all ftls */
    if (core.run_flag & RUN_FTL) {
        while (core.ftl_count) {
            ftl = LIST_FIRST(&ftl_head);
            nvm_unregister_ftl(ftl);
        };
        core.run_flag ^= RUN_FTL;
    }

    /* Clean all media managers */
    if (core.run_flag & RUN_MMGR) {
        while (core.mmgr_count) {
            mmgr = LIST_FIRST(&mmgr_head);
            nvm_unregister_mmgr(mmgr);
        };
        core.run_flag ^= RUN_MMGR;
    }

    /* Clean Nvme */
    if((core.run_flag & RUN_NVME) && core.nvm_nvme_ctrl->num_namespaces) {
        core.nvm_nvme_ctrl->running = 1; /* not ready */
        usleep (100);
        nvme_exit();
        core.run_flag ^= RUN_NVME;
    }
    if ((core.run_flag & RUN_NVME_ALLOC) && stop_all) {
        free(core.nvm_nvme_ctrl);
        core.run_flag ^= RUN_NVME_ALLOC;
    }

    printf("OX Controller closed succesfully.\n");
}

int nvm_restart (void)
{
    int ret;
    nvm_clear_all (NVM_RESTART);
    if ((ret = nvm_init (NVM_RESTART) == 0))
        core.nvm_pcie->ops->reset();

    return ret;
}

static void nvm_printerror (int error)
{
    switch (error) {
        case EMAX_NAME_SIZE:
            log_err ("nvm: Name size out of bounds.\n");
            break;
        case EMMGR_REGISTER:
            log_err ("nvm: Error when register mmgr.\n");
            break;
        case EFTL_REGISTER:
            log_err ("nvm: Error when register FTL.\n");
            break;
        case EPCIE_REGISTER:
            log_err ("nvm: Error when register PCIe interconnection.\n");
            break;
        case ENVME_REGISTER:
            log_err ("nvm: Error when start NVMe standard.\n");
            break;
        case ECH_CONFIG:
            log_err ("nvm: a channel is not set correctly.\n");
            break;
        case EMEM:
            log_err ("nvm: Memory error.\n");
            break;
        default:
            log_err ("nvm: unknown error.\n");
    }
}

static void nvm_print_log (void)
{
    log_info("[nvm: OX Controller ready.]\n");
    log_info("  [nvm: Active pci handler: %s]\n", core.nvm_pcie->name);
    log_info("  [nvm: %d media manager(s) up, total of %d channels]\n",
                                           core.mmgr_count, core.nvm_ch_count);

    int i;
    for(i=0; i<core.nvm_ch_count; i++){
        log_info("    [channel: %d, FTL id: %d"
                            " ns_id: %d, ns_part: %d, pg: %lu, in_use: %d]\n",
                            core.nvm_ch[i]->ch_id, core.nvm_ch[i]->i.ftl_id,
                            core.nvm_ch[i]->i.ns_id, core.nvm_ch[i]->i.ns_part,
                            core.nvm_ch[i]->ns_pgs, core.nvm_ch[i]->i.in_use);
    }
    log_info("  [nvm: namespace size: %lu bytes]\n",
                                                core.nvm_nvme_ctrl->ns_size[0]);
    log_info("    [nvm: total pages: %lu]\n",
                                  core.nvm_nvme_ctrl->ns_size[0] / NVM_PG_SIZE);
}

static void nvm_jump (int signal) {
    longjmp (core.jump, 1);
}

int nvm_memcheck (void *mem) {
    int invalid = 0;
    signal (SIGSEGV, nvm_jump);
    if (setjmp (core.jump))
        invalid = 1;
    signal (SIGSEGV, SIG_DFL);
    return invalid;
}

int nvm_contains_ppa (struct nvm_ppa_addr *list, uint32_t list_sz,
                                                        struct nvm_ppa_addr ppa)
{
    int i;

    if (!list)
        return 0;

    for (i = 0; i < list_sz; i++)
        if (ppa.ppa == list[i].ppa)
            return 1;

    return 0;
}

int nvm_test_unit (struct nvm_init_arg *args)
{
    if (core.tests_init->init)
        return core.tests_init->init(args);
    else
        printf(" Tests are not compiled.\n");

    return -1;
}

int nvm_admin_unit (struct nvm_init_arg *args)
{
    if (!core.tests_init->admin) {
        printf(" Ox Administration is not compiled.\n");
        return -1;
    }
    return 0;
}

int nvm_init_ctrl (int argc, char **argv, QemuOxCtrl *qemu)
{
    int ret, exec;
    pthread_t mode_t;
    void *(*modet_fn)(void *);

    core.tests_init = &tests_is;

    exec = cmdarg_init(argc, argv);
    if (exec < 0)
        goto OUT;

    openlog("NVME",LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL0);
    LIST_INIT(&mmgr_head);
    LIST_INIT(&ftl_head);

    printf("OX Controller %s - %s\n Starting...\n", OX_VER, LABEL);
    log_info("[nvm: OX Controller is starting...]\n");

    core.qemu = qemu;
    ret = nvm_init(NVM_FULL_UPDOWN);
    if(ret)
        goto CLEAN;

    nvm_print_log();

    core.run_flag |= RUN_TESTS;
    switch (exec) {
        case OX_TEST_MODE:
            modet_fn = core.tests_init->start;
            break;
        case OX_ADMIN_MODE:
            modet_fn = core.tests_init->admin;
            break;
        case OX_RUN_MODE:
            core.run_flag ^= RUN_TESTS;
           // while(1) { usleep(1); } break;
            return 0;
        default:
            goto CLEAN;
    }

    if (core.tests_init->init) {
        pthread_create(&mode_t, NULL, modet_fn, NULL);
        pthread_join(mode_t, NULL);
    }

    goto OUT;
CLEAN:
    printf(" ERROR. Aborting. Check log file.\n");
    nvm_printerror(ret);
    nvm_clear_all(NVM_FULL_UPDOWN);
    return -1;
OUT:
    nvm_clear_all(NVM_FULL_UPDOWN);
    return 0;
}