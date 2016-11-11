#include <stdio.h>
#include <pthread.h>
#include <mqueue.h>
#include <syslog.h>
#include "volt.h"
#include "hw/block/ox-ctrl/include/ssd.h"

static VoltCtrl *volt;
static struct nvm_mmgr volt_mmgr;
extern struct core_struct core;

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

static void volt_callback (struct nvm_mmgr_io_cmd *cmd)
{
    nvm_callback(cmd);
}

static int volt_process_io (struct nvm_mmgr_io_cmd *cmd)
{
    cmd->status = NVM_IO_SUCCESS;
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
            volt_callback(cmd); /* error */
            continue;
        }

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

static int volt_init(void)
{
    int pages_ok;
    int res_l;
    int res_c;

    volt = malloc (sizeof (VoltCtrl));
    if (!volt)
        return -1;

    volt->status.allocated_memory = 0;

    /* Memory allocation. For now only LUNs, blocks and pages */
    pages_ok = volt_init_blocks();
    res_l = volt_init_luns();
    res_c = volt_init_channels();

    if (!pages_ok || !res_l || !res_c)
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

static int volt_read_page (struct nvm_mmgr_io_cmd *cmd)
{
    volt_enqueue_io (cmd);

    // VERIFY ADDRESS
    // VERIFY PAGE STATE

    //VoltBlock *blk = volt_get_block(addr);
    //memcpy(/*prp*/,&blk->data[addr.g.pg * volt_mmgr.geometry->pg_size],/*len*/);

    //printf("15 first chars read: %.15s\n",(char *)/*prp*/);

    return 0;
}

static int volt_write_page (struct nvm_mmgr_io_cmd *cmd)
{
    volt_enqueue_io (cmd);

    // VERIFY ADDRESS
    // VERIFY SIZE <= PAGE_SIZE (FOR NOW ONLY WRITES OF THE PAGE SIZE)
    // VERIFY SEQUENTIAL PAGE WITHIN THE BLOCK
    // VERIFY IF THE BLOCK IS FULL
    // VERIFY PAGE STATE
    // MODIFY PAGE STATE
    // MODIFY NEXT PAGE

    //VoltBlock *blk = volt_get_block(addr);
    //memcpy(&blk->data[addr.g.pg * volt_mmgr.geometry->pg_size],/*prp*/,/*len*/);

   // printf("15 first chars written: %.15s\n",
   //                (char *)&blk->data[addr.g.pg * volt_mmgr.geometry->pg_size]);

    return 0;
}

static int volt_erase_blk (struct nvm_mmgr_io_cmd *cmd)
{
    volt_enqueue_io (cmd);
    return 0;
}

static void volt_exit (struct nvm_mmgr *mmgr)
{
    volt_clean_mem();
    volt->status.active = 0;
    mq_close(volt->mq_id);
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
