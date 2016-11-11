#include <stdio.h>
#include <pthread.h>
#include <mqueue.h>
#include <syslog.h>
#include "volt.h"
#include "hw/block/ox-ctrl/include/ssd.h"
#include "hw/block/ox-ctrl/include/uatomic.h"

static u_atomic_t       nextprp;
static pthread_mutex_t  prp_mutex;
static pthread_mutex_t  prpmap_mutex;
static uint8_t          *prp_map;

static VoltCtrl             *volt;
static struct nvm_mmgr      volt_mmgr;
static void                 **dma_buf;
extern struct core_struct   core;

static int volt_start_prp_map(void)
{
    nextprp.counter = U_ATOMIC_INIT_RUNTIME(0);
    pthread_mutex_init (&prp_mutex, NULL);
    pthread_mutex_init (&prpmap_mutex, NULL);
    prp_map = calloc(sizeof(uint8_t), 2048);
    if (!prp_map)
        return EMEM;

    return 0;
}

static void volt_set_prp_map(uint32_t index, uint8_t flag)
{
    pthread_mutex_lock(&prpmap_mutex);
    prp_map[index / 8] = (flag)
            ? prp_map[index / 8] | 1 << (index % 8)
            : prp_map[index / 8] ^ 1 << (index % 8);
    pthread_mutex_unlock(&prpmap_mutex);
}

static uint32_t volt_get_next_prp(struct volt_dma *dma){
    uint16_t ni = VOLT_DMA_SLOT_INDEX;
    uint16_t noff = VOLT_DMA_SLOT_OFF;
    uint32_t next = 0;

    do {
        pthread_mutex_lock(&prp_mutex);
        next = u_atomic_read(&nextprp);

        if (next >= ni*noff)
            next = 1;
        else
            next++;

        u_atomic_set(&nextprp, next);
        next--;
        if (prp_map[next / 8] & 1 << (next % 8)) {
            pthread_mutex_unlock (&prp_mutex);
            usleep(1);
            continue;
        }
        pthread_mutex_unlock (&prp_mutex);

        volt_set_prp_map(next, 0x1);

        dma->prp_index = next;

        // vector index (16 bits) + offset index (16 bits)
        return ((next / noff)+1) << 16 | (next % noff);
    } while (1);
}

/*static VoltBlock *volt_get_block(struct nvm_ppa_addr addr){
    return volt->luns[addr.g.lun].blk_offset + addr.g.blk;
}*/

static size_t volt_add_mem(int64_t bytes)
{
    volt->status.allocated_memory += bytes;
    return bytes;
}

static void volt_sub_mem(int64_t bytes)
{
    volt->status.allocated_memory -= bytes;
}

static VoltPage *volt_init_page(VoltPage *pg)
{
    pg->state = 0;
    return ++pg;
}

static int volt_init_blocks(void)
{
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;

    int page_count = 0;
    int i_blk;
    int i_pg;
    int total_blk = geo->n_of_planes * geo->blk_per_lun * geo->lun_per_ch *
                                                                   geo->n_of_ch;

    volt->blocks = g_malloc(volt_add_mem (sizeof(VoltBlock) * total_blk));
    if (!volt->blocks)
        return VOLT_MEM_ERROR;

    for (i_blk = 0; i_blk < total_blk; i_blk++) {
        VoltBlock *blk = &volt->blocks[i_blk];
        blk->id = i_blk;
        blk->life = VOLT_BLK_LIFE;

        blk->pages = g_malloc(volt_add_mem(sizeof(VoltPage) * geo->pg_per_blk));
        if (!blk->pages)
            return VOLT_MEM_ERROR;

        blk->next_pg = blk->pages;

        blk->data = g_malloc0(volt_add_mem(geo->pg_size * geo->pg_per_blk));
        if (!blk->data)
            return VOLT_MEM_ERROR;

        VoltPage *pg = blk->pages;
        for (i_pg = 0; i_pg < geo->pg_per_blk; i_pg++) {
            pg = volt_init_page(pg);
            page_count++;
        }
    }
    return page_count;
}

static int volt_init_luns(void)
{
    int i_lun;
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;

    volt->luns = g_malloc(volt_add_mem(sizeof (VoltLun) * geo->lun_per_ch));
    if (!volt->luns)
        return VOLT_MEM_ERROR;

    for (i_lun = 0; i_lun < geo->lun_per_ch; i_lun++) {
        volt->luns[i_lun].blk_offset = &volt->blocks[i_lun * geo->blk_per_lun];
    }
    return VOLT_MEM_OK;
}

static int volt_init_channels(void)
{
    int i_ch;
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;

    volt->channels = g_malloc(volt_add_mem(sizeof (VoltCh) * geo->n_of_ch));
    if (!volt->channels)
        return VOLT_MEM_ERROR;

    for (i_ch = 0; i_ch < geo->n_of_ch; i_ch++) {
        volt->channels[i_ch].lun_offset = &volt->luns[i_ch * geo->lun_per_ch];
    }
    return VOLT_MEM_OK;
}

static void volt_clean_mem(void)
{
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;

    int total_blk = geo->blk_per_lun * geo->lun_per_ch;

    int i;
    for (i = 0; i < total_blk; i++) {
        g_free(volt->blocks[i].data);
        volt_sub_mem(geo->pg_size * geo->pg_per_blk);

        g_free(volt->blocks[i].pages);
        volt_sub_mem(sizeof (VoltPage) * geo->pg_per_blk);

    }
    g_free(volt->blocks);
    volt_sub_mem(sizeof (VoltBlock) * total_blk);

    g_free(volt->luns);
    volt_sub_mem(sizeof (VoltLun) * geo->lun_per_ch);
}

static int volt_dma_helper (struct nvm_mmgr_io_cmd *nvm_cmd)
{
    uint32_t dma_sz;
    int dma_sec, c = 0, ret = 0;
    uint64_t prp;
    uint8_t direction;
    struct volt_dma *dma = (struct volt_dma *) nvm_cmd->rsvd;

    switch (nvm_cmd->cmdtype) {
        case MMGR_READ_PG:
            direction = (nvm_cmd->sync_count) ? NVM_DMA_SYNC_READ :
                                                             NVM_DMA_TO_HOST;
            break;
        case MMGR_WRITE_PG:
            direction = (nvm_cmd->sync_count) ? NVM_DMA_SYNC_WRITE :
                                                             NVM_DMA_FROM_HOST;
            break;
        default:
            return -1;
    }

    dma_sec = nvm_cmd->n_sectors + 1;
    for (; c < dma_sec; c++) {
        dma_sz = (c == dma_sec - 1) ? nvm_cmd->md_sz : nvm_cmd->sec_sz;
        prp = (c == dma_sec - 1) ? nvm_cmd->md_prp : nvm_cmd->prp[c];

        ret = nvm_dma ((void *)(dma->virt_addr +
                                nvm_cmd->sec_sz * c), prp, dma_sz, direction);
        if (ret) break;
    }

    return ret;
}

static void volt_callback (struct nvm_mmgr_io_cmd *nvm_cmd)
{
    int ret = 0;
    struct volt_dma *dma = (struct volt_dma *) nvm_cmd->rsvd;

    if (!nvm_cmd || nvm_cmd->status == NVM_IO_TIMEOUT)
        goto OUT;

    if (dma->status) {
        if (nvm_cmd->cmdtype == MMGR_READ_PG)
            ret = volt_dma_helper (nvm_cmd);

        nvm_cmd->status = (ret) ? NVM_IO_FAIL : NVM_IO_SUCCESS;
    } else {
        nvm_cmd->status = NVM_IO_FAIL;
    }

OUT:
    if (nvm_cmd->cmdtype == MMGR_WRITE_PG || nvm_cmd->cmdtype == MMGR_READ_PG)
        volt_set_prp_map(dma->prp_index, 0x0);

    nvm_callback(nvm_cmd);
}

static int volt_process_io (struct nvm_mmgr_io_cmd *cmd)
{
    //VoltBlock *blk = volt_get_block(addr); write
    //memcpy(&blk->data[addr.g.pg * volt_mmgr.geometry->pg_size],/*prp*/,/*len*/);

   // printf("15 first chars written: %.15s\n",
   //                (char *)&blk->data[addr.g.pg * volt_mmgr.geometry->pg_size]);


    // VERIFY ADDRESS read
    // VERIFY PAGE STATE

    //VoltBlock *blk = volt_get_block(addr);
    //memcpy(/*prp*/,&blk->data[addr.g.pg * volt_mmgr.geometry->pg_size],/*len*/);

    //printf("15 first chars read: %.15s\n",(char *)/*prp*/);
    struct volt_dma *dma = (struct volt_dma *) cmd->rsvd;
    dma->status = 1;

    return 0;
}

static void *volt_io_thread (void *arg)
{
    struct nvm_mmgr_io_cmd *cmd;
    uint64_t *cmd_addr;
    int ret;

    volt->status.active = 1;
    do {
        cmd = 0;
        ret = mq_receive (volt->mq_id, (char *)&cmd_addr,
                                                      sizeof (uint64_t), NULL);
        if (ret != VOLT_MQ_MSGSIZE)
            continue;

        cmd = (struct nvm_mmgr_io_cmd *) cmd_addr;
	if(cmd == NULL)
            continue;

        ret = volt_process_io(cmd);

        if (ret) {
            log_err ("[ERROR: Cmd %x not completed. Aborted.]\n", cmd->cmdtype);
            cmd->status = NVM_IO_FAIL;
            volt_callback(cmd); /* error */
            continue;
        }

        cmd->status = NVM_IO_SUCCESS;
        volt_callback(cmd); /* success */

    } while (volt->status.active);

    return NULL;
}

static int volt_enqueue_io (struct nvm_mmgr_io_cmd *io)
{
    int ret, retry;
    uint64_t cmd_addr;

    cmd_addr = (uint64_t) io;

    retry = 16;
    do {
        ret = mq_send(volt->mq_id, (char *) &cmd_addr, sizeof (uint64_t), 1);
    	if (ret < 0)
            retry--;
        else if (core.debug)
            printf(" MMGR_CMD type: 0x%x submitted to VOLT.\n  "
                    "Channel: %d, lun: %d, blk: %d, pl: %d, "
                    "pg: %d]\n", io->cmdtype, io->ppa.g.ch, io->ppa.g.lun,
                    io->ppa.g.blk, io->ppa.g.pl, io->ppa.g.pg);

    } while (ret < 0 && retry);

    return (retry) ? 0 : -1;
}

static int volt_init_dma_buf (void)
{
    int slots;

    dma_buf = g_malloc(sizeof (void *) * VOLT_DMA_SLOT_INDEX);
    if (!dma_buf)
        return -1;

    for (slots = 0; slots < VOLT_DMA_SLOT_INDEX; slots++) {
        dma_buf[slots] = g_malloc(volt_add_mem
                       (VOLT_DMA_SLOT_OFF * (VOLT_PAGE_SIZE + VOLT_OOB_SIZE)));
    }

    return 0;
}

static void volt_free_dma_buf (void)
{
    int slots;

    for (slots = 0; slots < VOLT_DMA_SLOT_INDEX; slots++) {
        g_free(dma_buf[slots]);
        volt_sub_mem(VOLT_DMA_SLOT_OFF * (VOLT_PAGE_SIZE + VOLT_OOB_SIZE));
    }
    g_free(dma_buf);
}

static int volt_prepare_rw (struct nvm_mmgr_io_cmd *cmd_nvm)
{
    int c;
    struct volt_dma *dma = (struct volt_dma *) cmd_nvm->rsvd;

    uint32_t sec_sz = volt_mmgr.geometry->pg_size /
                                                volt_mmgr.geometry->sec_per_pg;
    uint32_t pg_sz  = volt_mmgr.geometry->pg_size;
    uint32_t oob_sz = volt_mmgr.geometry->sec_oob_sz *
                                                volt_mmgr.geometry->sec_per_pg;

    // For now we only accept up to 16K page size and 4K sector size + 1K OOB
    if (cmd_nvm->pg_sz > pg_sz || cmd_nvm->sec_sz != sec_sz ||
                                                    cmd_nvm->md_sz > oob_sz)
        return -1;

    memset(dma, 0, sizeof(struct volt_dma));

    uint32_t prp_map = volt_get_next_prp(dma);
    uint16_t vec_index = prp_map >> 16;
    uint32_t offset = (prp_map & 0x0000ffff) * sec_sz * 5;

    dma->virt_addr = dma_buf[vec_index] + offset;

    if (cmd_nvm->cmdtype == MMGR_READ_PG)
        memset(dma->virt_addr, 0, pg_sz + oob_sz);

    cmd_nvm->n_sectors = cmd_nvm->pg_sz / sec_sz;

    if (cmd_nvm->cmdtype == MMGR_WRITE_PG) {
        if (volt_dma_helper (cmd_nvm))
            return -1;
    }

    for (c = 0; c < 5; c++) {
        dma->len[c] = sec_sz;
        dma->host_addr[c] = ((uint64_t)&dma_buf[vec_index])+offset + sec_sz * c;
    }
    dma->len[4] = cmd_nvm->md_sz;

    return 0;
}

static int volt_read_page (struct nvm_mmgr_io_cmd *cmd_nvm)
{
    struct volt_dma *dma = (struct volt_dma *) cmd_nvm->rsvd;

    if (volt_prepare_rw(cmd_nvm))
        goto CLEAN;

    if (volt_enqueue_io (cmd_nvm))
        goto CLEAN;

    return 0;

CLEAN:
    log_err("[MMGR Read ERROR: NVM  returned -1]\n");
    volt_set_prp_map(dma->prp_index, 0x0);
    cmd_nvm->status = NVM_IO_FAIL;
    return -1;
}

static int volt_write_page (struct nvm_mmgr_io_cmd *cmd_nvm)
{
    struct volt_dma *dma = (struct volt_dma *) cmd_nvm->rsvd;

    if (volt_prepare_rw(cmd_nvm))
        goto CLEAN;

    if(volt_enqueue_io (cmd_nvm))
        goto CLEAN;

    return 0;

CLEAN:
    log_err("[MMGR Write ERROR: DMA or NVM returned -1]\n");
    volt_set_prp_map(dma->prp_index, 0x0);
    cmd_nvm->status = NVM_IO_FAIL;
    return -1;
}

static int volt_erase_blk (struct nvm_mmgr_io_cmd *cmd)
{
    if(volt_enqueue_io (cmd))
        goto CLEAN;

    return 0;

CLEAN:
    log_err("[MMGR Erase ERROR: NVM library returned -1]\n");
    cmd->status = NVM_IO_FAIL;
    return -1;
}

static void volt_exit (struct nvm_mmgr *mmgr)
{
    int i;
    volt_clean_mem();
    volt_free_dma_buf();
    volt->status.active = 0;
    mq_close(volt->mq_id);
    pthread_mutex_destroy(&prp_mutex);
    pthread_mutex_destroy(&prpmap_mutex);
    free(prp_map);
    for (i = 0; i < mmgr->geometry->n_of_ch; i++) {
        free(mmgr->ch_info->mmgr_rsv_list);
        free(mmgr->ch_info->ftl_rsv_list);
    }
    free (volt);
}

static int volt_set_ch_info (struct nvm_channel *ch, uint16_t nc)
{
    return 0;
}

static int volt_get_ch_info (struct nvm_channel *ch, uint16_t nc)
{
    int i, n, nsp = 0;

    for(i = 0; i < nc; i++){
        ch[i].ch_mmgr_id = i;
        ch[i].mmgr = &volt_mmgr;
        ch[i].geometry = volt_mmgr.geometry;

       // if (dfcnand_read_nvminfo (&ch[i]))
       //     return -1;

        if (ch[i].i.in_use != NVM_CH_IN_USE) {
            ch[i].i.ns_id = 0x0;
            ch[i].i.ns_part = 0x0;
            ch[i].i.ftl_id = 0x0;
            ch[i].i.in_use = 0x0;
        }

        ch[i].ns_pgs = VOLT_VIRTUAL_LUNS *
                       VOLT_BLOCK_COUNT *
                       VOLT_PLANE_COUNT *
                       VOLT_PAGE_COUNT;

        ch[i].mmgr_rsv = 3;
        ch[i].mmgr_rsv_list = malloc (ch[i].mmgr_rsv *
                                                sizeof(struct nvm_ppa_addr));
        if (!ch[i].mmgr_rsv_list)
            return EMEM;

        for (n = 0; n < ch[i].mmgr_rsv; n++)
            ch[i].mmgr_rsv_list[n].ppa = (uint64_t) (n & 0xffffffffffffffff);

        ch[i].ftl_rsv = 0;
        ch[i].ftl_rsv_list = malloc (sizeof(struct nvm_ppa_addr));
        if (!ch[i].ftl_rsv_list)
            return EMEM;

        ch[i].tot_bytes = 0;
        ch[i].slba = 0;
        ch[i].elba = 0;
        nsp++;
    }

    return 0;
}

static int volt_init(void)
{
    int pages_ok;
    int res_l;
    int res_c;
    int ret;

    volt = malloc (sizeof (VoltCtrl));
    if (!volt)
        return -1;

    volt->status.allocated_memory = 0;

    ret = volt_start_prp_map();
    if (ret)
        return ret;

    /* Memory allocation. For now only LUNs, blocks and pages */
    pages_ok = volt_init_blocks();
    res_l = volt_init_luns();
    res_c = volt_init_channels();
    ret = volt_init_dma_buf();

    if (!pages_ok || !res_l || !res_c || ret)
        goto MEM_CLEAN;

    mq_unlink ("/volt");
    struct mq_attr mqAttr = {0, VOLT_MQ_MAXMSG, VOLT_MQ_MSGSIZE, 0};
    volt->mq_id = mq_open ("/volt", O_RDWR|O_CREAT, S_IWUSR|S_IRUSR, &mqAttr);
    if (volt->mq_id < 0)
	return -1;

    if(pthread_create(&volt->io_thread, NULL, volt_io_thread, NULL))
        return -1;

    log_info(" [volt: Volt IO Queue started.]\n");

    volt->status.ready = 1; /* ready to use */

    log_info(" [volt: Volatile memory usage: %lu Mb]\n",
                                      volt->status.allocated_memory / 1048576);
    printf(" [volt: Volatile memory usage: %lu Mb]\n",
                                      volt->status.allocated_memory / 1048576);
    return 0;

MEM_CLEAN:
    volt->status.ready = 0;
    volt_clean_mem();
    printf(" [volt: Not initialized! Memory allocation failed.]\n");
    printf(" [volt: Volatile memory usage: %lu bytes.]\n",
                                                volt->status.allocated_memory);
    return -1;
}

struct nvm_mmgr_ops volt_ops = {
    .write_pg       = volt_write_page,
    .read_pg        = volt_read_page,
    .erase_blk      = volt_erase_blk,
    .exit           = volt_exit,
    .get_ch_info    = volt_get_ch_info,
    .set_ch_info    = volt_set_ch_info,
};

struct nvm_mmgr_geometry volt_geo = {
    .n_of_ch        = VOLT_CHIP_COUNT,
    .lun_per_ch     = VOLT_VIRTUAL_LUNS,
    .blk_per_lun    = VOLT_BLOCK_COUNT,
    .pg_per_blk     = VOLT_PAGE_COUNT,
    .sec_per_pg     = VOLT_SECTOR_COUNT,
    .n_of_planes    = VOLT_PLANE_COUNT,
    .pg_size        = VOLT_PAGE_SIZE,
    .sec_oob_sz     = VOLT_OOB_SIZE / VOLT_SECTOR_COUNT
};

int mmgr_volt_init(void)
{
    int ret = 0;

    volt_mmgr.name     = "VOLT";
    volt_mmgr.ops      = &volt_ops;
    volt_mmgr.geometry = &volt_geo;

    ret = volt_init();
    if(ret) {
        log_err(" [volt: Not possible to start VOLT.]\n");
        return -1;
    }

    return nvm_register_mmgr(&volt_mmgr);
}
