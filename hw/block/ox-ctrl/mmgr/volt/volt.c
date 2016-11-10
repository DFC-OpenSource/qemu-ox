#include <stdio.h>
#include <pthread.h>
#include "volt.h"
#include "hw/block/ox-ctrl/include/ssd.h"

void nvme_volt_main(void *opaque)
{
    LnvmVoltCtrl *volt = (LnvmVoltCtrl *) opaque;

    if (volt->status.active) {
        printf("\nvolt: I am alive in ns%d, I yield here each 10 minutes!\n",volt->ns->id);
        timer_mod(volt->mainTimer, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + LNVM_VOLT_SECOND * 600);
    }
}

static LnvmVoltBlock *nvme_volt_get_block(LnvmVoltCtrl *volt, struct ppa_addr addr){
    return volt->luns[addr.g.lun].blk_offset+addr.g.blk;
}

static int nvme_volt_print_read(NvmeRequest *req, struct ppa_addr addr, QEMUIOVector *iov){
    LnvmVoltCtrl *volt = (LnvmVoltCtrl *) req->ns->ctrl->volt_ctrl;
    printf("Volt device is readind!\n");

    // VERIFY ADDRESS
    // VERIFY PAGE STATE

    LnvmVoltBlock *blk = nvme_volt_get_block(volt, addr);
    memcpy(iov->iov->iov_base,&blk->data[addr.g.pg * volt->params.pg_size],iov->iov->iov_len);
    printf("15 first chars read: %.15s\n",(char *)iov->iov->iov_base);

    return 0;
}

static int nvme_volt_print_write(NvmeRequest *req, struct ppa_addr addr, QEMUIOVector *iov){
    LnvmVoltCtrl *volt = (LnvmVoltCtrl *) req->ns->ctrl->volt_ctrl;
    printf("Volt device is writing!\n");

    // VERIFY ADDRESS
    // VERIFY SIZE <= PAGE_SIZE (FOR NOW ONLY WRITES OF THE PAGE SIZE)
    // VERIFY SEQUENTIAL PAGE WITHIN THE BLOCK
    // VERIFY IF THE BLOCK IS FULL
    // VERIFY PAGE STATE
    // MODIFY PAGE STATE
    // MODIFY NEXT PAGE

    LnvmVoltBlock *blk = nvme_volt_get_block(volt, addr);
    memcpy(&blk->data[addr.g.pg * volt->params.pg_size],iov->iov->iov_base,iov->iov->iov_len);
    printf("15 first chars written: %.15s\n",(char *)&blk->data[addr.g.pg * volt->params.pg_size]);

    return 0;
}

static void nvme_volt_bh(void *opaque)
{
    LnvmVoltBlockAIOCBCoroutine *acb = opaque;

    acb->common.cb(acb->common.opaque, acb->req.error);

    qemu_bh_delete(acb->bh);
    qemu_aio_unref(acb);
}

void coroutine_fn nvme_volt_redirect_co(void *opaque){
    LnvmVoltBlockAIOCBCoroutine *acb = opaque;
    BlockDriverState *bs = acb->common.bs;
    NvmeRequest *req = (NvmeRequest *) ((LnvmVoltBlockAIOCBCoroutine *) acb->common.opaque)->common.opaque;

    struct ppa_addr lnvm_addr;
    lnvm_addr.ppa = acb->req.sector;
    printf("LightNVM address: %#018lx\n", lnvm_addr.ppa);
    printf("LightNVM address: blk:0x%04x, pg:0x%04x, sec:0x%02x, pl:0x%02x, lun:0x%02x, ch:0x%02x\n",
            lnvm_addr.g.blk, lnvm_addr.g.pg, lnvm_addr.g.sec, lnvm_addr.g.pl, lnvm_addr.g.lun, lnvm_addr.g.ch);

    if (!acb->is_write) {
        acb->req.error = nvme_volt_print_read(req, lnvm_addr, acb->req.qiov); // VOLT READ
    } else {
        acb->req.error = nvme_volt_print_write(req, lnvm_addr, acb->req.qiov); // VOLT WRITE
    }

    acb->bh = aio_bh_new(bdrv_get_aio_context(bs), nvme_volt_bh, acb);
    qemu_bh_schedule(acb->bh);
}

static const AIOCBInfo nvme_volt_aiocb_info = {
    .aiocb_size         = sizeof(LnvmVoltBlockAIOCBCoroutine),
};

static BlockAIOCB *nvme_volt_redirect_rw(BlockDriverState *bs,
                                         int64_t sector_num,
                                         QEMUIOVector *qiov,
                                         int nb_sectors,
                                         BdrvRequestFlags flags,
                                         BlockCompletionFunc *cb,
                                         void *opaque,
                                         bool is_write){
    Coroutine *co;
    LnvmVoltBlockAIOCBCoroutine *acb;

    acb = qemu_aio_get(&nvme_volt_aiocb_info, bs, cb, opaque);
    acb->req.sector = sector_num;
    acb->req.nb_sectors = nb_sectors;
    acb->req.qiov = qiov;
    acb->req.flags = flags;
    acb->is_write = is_write;

    co = qemu_coroutine_create(nvme_volt_redirect_co);
    qemu_coroutine_enter(co, acb);

    return &acb->common;
}

BlockAIOCB *nvme_volt_redirect_write(BlockBackend *blk, int64_t sector_num,
                           QEMUIOVector *iov, int nb_sectors,
                           BlockCompletionFunc *cb, void *opaque){
    return nvme_volt_redirect_rw(blk->bs, sector_num, iov, nb_sectors, 0, cb, opaque, true);
}

BlockAIOCB *nvme_volt_redirect_read(BlockBackend *blk, int64_t sector_num,
                           QEMUIOVector *iov, int nb_sectors,
                           BlockCompletionFunc *cb, void *opaque){
    return nvme_volt_redirect_rw(blk->bs, sector_num, iov, nb_sectors, 0, cb, opaque, false);
}

static size_t nvme_volt_add_mem(LnvmVoltCtrl *volt, int64_t bytes)
{
    volt->status.allocated_memory += bytes;
    return bytes;
}

static void nvme_volt_sub_mem(LnvmVoltCtrl *volt, int64_t bytes)
{
    volt->status.allocated_memory -= bytes;
}

static LnvmVoltPage * nvme_volt_init_page(LnvmVoltPage *pg, uint16_t pg_id)
{
    pg->state = 0;
    return ++pg;
}

static int nvme_volt_init_blocks(LnvmVoltCtrl *volt)
{
    int page_count = 0;
    int i_blk;
    int i_pg;
    int total_blk = volt->params.num_blk * volt->params.num_lun;

    volt->blocks = g_malloc(nvme_volt_add_mem(volt, sizeof(LnvmVoltBlock) * total_blk));
    if (!volt->blocks)
        return LNVM_VOLT_MEM_ERROR;
               
    for (i_blk = 0; i_blk < total_blk; i_blk++) {
        LnvmVoltBlock *blk = &volt->blocks[i_blk];
        blk->id = i_blk;
        blk->life = LNVM_VOLT_BLK_LIFE;

        blk->pages = g_malloc(nvme_volt_add_mem(volt, sizeof(LnvmVoltPage) * volt->params.num_pg));
        if (!blk->pages)
            return LNVM_VOLT_MEM_ERROR;
        blk->next_pg = blk->pages;

        blk->data = g_malloc0(nvme_volt_add_mem(volt, volt->params.pg_size * volt->params.num_pg));
        if (!blk->data)
            return LNVM_VOLT_MEM_ERROR;

        LnvmVoltPage *pg = blk->pages;
        for (i_pg = 0; i_pg < volt->params.num_pg; i_pg++) {
            pg = nvme_volt_init_page(pg, page_count);
            page_count++;
        }
    }
    return page_count;
}

static int nvme_volt_init_luns(LnvmVoltCtrl *volt)
{
    int i_lun;
    volt->luns = g_malloc(nvme_volt_add_mem(volt, sizeof (LnvmVoltLun) * volt->params.num_lun));
    if (!volt->luns)
        return LNVM_VOLT_MEM_ERROR;

    for (i_lun = 0; i_lun < volt->params.num_lun; i_lun++) {
        volt->luns[i_lun].blk_offset = &volt->blocks[i_lun * volt->params.num_blk];
    }
    return LNVM_VOLT_MEM_OK;
}

static void nvme_volt_clean_mem(LnvmVoltCtrl *volt)
{
    int total_blk = volt->params.num_blk * volt->params.num_lun;
    int i;
    for (i = 0; i < total_blk; i++) {
        g_free(volt->blocks[i].data);
        nvme_volt_sub_mem(volt, volt->params.pg_size * volt->params.num_pg);
        g_free(volt->blocks[i].pages);
        nvme_volt_sub_mem(volt, sizeof (LnvmVoltPage) * volt->params.num_pg);
    }
    g_free(volt->blocks);
    nvme_volt_sub_mem(volt, sizeof (LnvmVoltBlock) * total_blk);

    g_free(volt->luns);
    nvme_volt_sub_mem(volt, sizeof (LnvmVoltLun) * volt->params.num_lun);
}

static void volt_init(void)
{  
    volt->status.allocated_memory = 0;

    /* Memory allocation. For now only LUNs, blocks and pages */
    int pages_ok = nvme_volt_init_blocks(volt);
    int res = nvme_volt_init_luns(volt);

    if (!pages_ok || !res)
        goto MEM_CLEAN;        
        
    volt->status.ready = 1; /* ready to use */
        
    printf("volt: Volatile memory usage: %lu Mb\n", volt->status.allocated_memory / 1048576);
        
MEM_CLEAN:
    volt->status.ready = 0;
    nvme_volt_clean_mem(volt);
    printf("volt: Not initialized ns%d! Memory allocation failed.\n", volt->ns->id);
    printf("volt: Volatile memory usage: %lu bytes.\n", volt->status.allocated_memory);
}

static int volt_read_page (struct nvm_mmgr_io_cmd *cmd_nvm)
{
    
}

static int volt_write_page (struct nvm_mmgr_io_cmd *cmd_nvm)
{
    
}

static int volt_erase_blk (struct nvm_mmgr_io_cmd *cmd_nvm)
{
    
}

static void volt_exit (struct nvm_mmgr *mmgr)
{
    
}

static int volt_set_ch_info (struct nvm_channel *ch, uint16_t nc)
{
    return 0;
}

static int volt_get_ch_info (struct nvm_channel *ch, uint16_t nc)
{
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

    dfcnand.name     = "VOLT";
    dfcnand.ops      = &volt_ops;
    dfcnand.geometry = &volt_geo;

    ret = volt_init();
    if(ret) {
        syslog(LOG_ERR, "volt: Not possible to start VOLT.");
        return -1;
    }
    //ret = dfcnand_start_prp_map();
    //if (ret) return ret;

    return nvm_register_mmgr(&volt);
}