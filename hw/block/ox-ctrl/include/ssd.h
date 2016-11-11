/* OX: OpenChannel NVM Express SSD Controller
 *
 * Copyright (C) 2016, IT University of Copenhagen. All rights reserved.
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 *
 * Funding support provided by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#ifndef SSD_H
#define SSD_H

#ifndef __USE_GNU
#define __USE_GNU
#endif

#define LIGHTNVM            1
#define INIT_DFC            1
#define MMGR_DFCNAND        0
#define MMGR_VOLT           1
#define FTL_LNVMFTL         1

#include <sys/queue.h>
#include <stdint.h>
#include <pthread.h>
#include <setjmp.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include "hw/block/ox-ctrl/include/uatomic.h"
#include "qemu/osdep.h"
#include "hw/block/block.h"
#include "hw/pci/msix.h"

#define TYPE_OX "ox-ctrl"
#define OXCTRL(obj) \
        OBJECT_CHECK(QemuOxCtrl, (obj), TYPE_OX)

#define MAX_NAME_SIZE           31
#define NVM_FTL_MQ             "/nvm_mq"
#define NVM_FTL_MQ_MSGSIZE     8
#define NVM_FTL_MQ_MAXMSG      64

#define NVM_SYNCIO_TO          10
#define NVM_SYNCIO_FLAG_BUF    0x1
#define NVM_SYNCIO_FLAG_SYNC   0x2
#define NVM_SYNCIO_FLAG_DEC    0x4

/* All media managers must accept page r/w of NVM_PG_SIZE + OOB_SIZE*/
#define NVM_PG_SIZE            0x4000
#define NVM_OOB_BITS           10
#define NVM_OOB_SIZE           (1 << NVM_OOB_BITS)

#define AND64                  0xffffffffffffffff
#define ZERO_32FLAG            0x00000000

#define NVM_CH_IN_USE          0x3c

#define FTL_ID_LNVM            0x1
#define FTL_ID_STANDARD        FTL_ID_LNVM

#define log_err(format, ...)         syslog(LOG_ERR, format, ## __VA_ARGS__)
#define log_info(format, ...)        syslog(LOG_INFO, format, ## __VA_ARGS__)

struct nvm_ppa_addr {
    /* Generic structure for all addresses */
    union {
        struct {
            uint64_t blk    : 16;
            uint64_t pg     : 16;
            uint64_t sec    : 8;
            uint64_t pl     : 8;
            uint64_t lun    : 8;
            uint64_t ch     : 8;
        } g;

        uint64_t ppa;
    };
};

struct nvm_memory_region {
    uint64_t     addr;
    uint64_t     size;
    uint8_t      is_valid;
};

struct nvm_channel;

struct nvm_io_status {
    uint8_t     status;       /* global status for the cmd */
    uint16_t    nvme_status;  /* status to send to host */
    uint32_t    pg_errors;    /* n of errors */
    uint32_t    total_pgs;
    uint16_t    pgs_p;        /* pages processed within iteration */
    uint16_t    pgs_s;        /* pages success in request */
    uint16_t    ret_t;        /* retried times */
    uint8_t     pg_map[8];    /* pgs to retry */
};

struct nvm_mmgr_io_cmd {
    struct nvm_io_cmd       *nvm_io;
    struct nvm_ppa_addr     ppa;
    uint64_t                prp[32]; /* max of 32 sectors */
    uint64_t                md_prp;
    uint8_t                 status;
    uint8_t                 cmdtype;
    uint32_t                pg_index; /* pg index inside nvm_io_cmd */
    uint32_t                pg_sz;
    uint16_t                n_sectors;
    uint32_t                sec_sz;
    uint32_t                md_sz;
    u_atomic_t              *sync_count;
    pthread_mutex_t         *sync_mutex;
    struct timeval          tstart;
    struct timeval          tend;

    /* DFC specific */
    uint8_t                 fpga_io[170];
};

struct nvm_io_cmd {
    uint64_t                    cid;
    struct nvm_channel          *channel;
    struct nvm_ppa_addr         ppalist[64];
    struct nvm_io_status        status;
    struct nvm_mmgr_io_cmd      mmgr_io[64];
    void                        *req;
    uint64_t                    prp[64];
    uint64_t                    md_prp[64];
    uint32_t                    sec_sz;
    uint32_t                    md_sz;
    uint32_t                    n_sec;
    uint64_t                    slba;
    uint8_t                     cmdtype;
    /* if the plane_page is not full, sec_offset means the number sectors
     *                                                      to be transfered */
    uint16_t                    sec_offset;
};

#include "hw/block/ox-ctrl/include/nvme.h"

struct NvmeCtrl;
struct NvmeCmd;
struct NvmeCtrl;
struct NvmeRequest;
union NvmeRegs;

enum {
    NVM_DMA_TO_HOST        = 0x0,
    NVM_DMA_FROM_HOST      = 0x1,
    NVM_DMA_SYNC_READ      = 0x2,
    NVM_DMA_SYNC_WRITE     = 0x3
};

enum {
    MMGR_READ_PG =   0x1,
    MMGR_READ_OOB =  0x2,
    MMGR_WRITE_PG =  0x3,
    MMGR_BAD_BLK =   0x5,
    MMGR_ERASE_BLK = 0x7,
};

enum NVM_ERROR {
    EMAX_NAME_SIZE   = 0xa,
    EMMGR_REGISTER   = 0xb,
    EPCIE_REGISTER   = 0xc,
    EFTL_REGISTER    = 0xd,
    ENVME_REGISTER   = 0x9,
    ECH_CONFIG       = 0xe,
    EMEM             = 0xf,
};

enum {
    NVM_IO_SUCCESS     = 0x1,
    NVM_IO_FAIL        = 0x2,
    NVM_IO_PROCESS     = 0x3,
    NVM_IO_NEW         = 0x4,
    NVM_IO_TIMEOUT     = 0x5,
};

enum RUN_FLAGS {
    RUN_NVME_ALLOC = 1 << 0,
    RUN_MMGR       = 1 << 1,
    RUN_FTL        = 1 << 2,
    RUN_CH         = 1 << 3,
    RUN_PCIE       = 1 << 4,
    RUN_NVME       = 1 << 5,
    RUN_TESTS      = 1 << 6,
};

struct nvm_mmgr;
typedef int     (nvm_mmgr_read_pg)(struct nvm_mmgr_io_cmd *);
typedef int     (nvm_mmgr_write_pg)(struct nvm_mmgr_io_cmd *);
typedef int     (nvm_mmgr_erase_blk)(struct nvm_mmgr_io_cmd *);
typedef int     (nvm_mmgr_get_ch_info)(struct nvm_channel *, uint16_t);
typedef int     (nvm_mmgr_set_ch_info)(struct nvm_channel *, uint16_t);
typedef void    (nvm_mmgr_exit)(struct nvm_mmgr *);

struct nvm_mmgr_ops {
    nvm_mmgr_read_pg       *read_pg;
    nvm_mmgr_write_pg      *write_pg;
    nvm_mmgr_erase_blk     *erase_blk;
    nvm_mmgr_exit          *exit;
    nvm_mmgr_get_ch_info   *get_ch_info;
    nvm_mmgr_set_ch_info   *set_ch_info;
};

struct nvm_mmgr_geometry {
    uint8_t     n_of_ch;
    uint8_t     lun_per_ch;
    uint16_t    blk_per_lun;
    uint16_t    pg_per_blk;
    uint16_t    sec_per_pg;
    uint8_t     n_of_planes;
    uint32_t    pg_size;
    uint32_t    sec_oob_sz;
};

struct nvm_mmgr {
    const char                  *name;
    struct nvm_mmgr_ops         *ops;
    struct nvm_mmgr_geometry    *geometry;
    struct nvm_channel          *ch_info;
    LIST_ENTRY(nvm_mmgr)        entry;
};

typedef void        (nvm_pcie_isr_notify)(void *);
typedef void        (nvm_pcie_exit)(void);
typedef void        *(nvm_pcie_nvme_consumer) (void *);

struct nvm_pcie_ops {
    nvm_pcie_nvme_consumer  *nvme_consumer;
    nvm_pcie_isr_notify     *isr_notify; /* notify host about completion */
    nvm_pcie_exit           *exit;
};

struct nvm_pcie {
    const char                  *name;
    void                        *ctrl;            /* pci specific structure */
    union NvmeRegs              *nvme_regs;
    struct nvm_pcie_ops          *ops;
    struct nvm_memory_region    *host_io_mem;     /* host BAR */
    pthread_t                   io_thread;        /* single thread for now */
    uint32_t                    *io_dbstride_ptr; /* for queue scheduling */
};

/* --- FTL CAPABILITIES BIT OFFSET --- */

enum {
    /* Get/Set Bad Block Table support */
    FTL_CAP_GET_BBTBL     = 0x00,
    FTL_CAP_SET_BBTBL     = 0x01,
    /* Get/Set Logical to Physical Table support */
    FTL_CAP_GET_L2PTBL    = 0x02,
    FTL_CAP_SET_L2PTBL    = 0x03,
};

/* --- FTL BAD BLOCK TABLE FORMATS --- */

enum {
   /* Each block within a LUN is represented by a byte, the function must return
   an array of n bytes, where n is the number of blocks per LUN. The function
   must set single bad blocks or accept an array of blocks to set as bad. */
    FTL_BBTBL_BYTE     = 0x00,
};

struct nvm_ftl;
typedef int       (nvm_ftl_submit_io)(struct nvm_io_cmd *);
typedef void      (nvm_ftl_callback_io)(struct nvm_mmgr_io_cmd *);
typedef int       (nvm_ftl_init_channel)(struct nvm_channel *);
typedef void      (nvm_ftl_exit)(struct nvm_ftl *);
typedef int       (nvm_ftl_get_bbtbl)(struct nvm_ppa_addr *,uint8_t *,uint32_t);
typedef int       (nvm_ftl_set_bbtbl)(struct nvm_ppa_addr *, uint32_t);

struct nvm_ftl_ops {
    nvm_ftl_submit_io      *submit_io; /* FTL queue request consumer */
    nvm_ftl_callback_io    *callback_io;
    nvm_ftl_init_channel   *init_ch;
    nvm_ftl_exit           *exit;
    nvm_ftl_get_bbtbl      *get_bbtbl;
    nvm_ftl_set_bbtbl      *set_bbtbl;
};

struct nvm_ftl {
    uint16_t                ftl_id;
    const char              *name;
    struct nvm_ftl_ops      *ops;
    uint32_t                cap; /* Capability bits */
    uint16_t                bbtbl_format;
    uint8_t                 nq; /* Number of queues/threads, up to 64 per FTL */
    int16_t                 mq_id[64];
    pthread_t               io_thread[64];
    uint8_t                 last_q;
    pthread_mutex_t         q_mutex;
    uint8_t                 active;
    LIST_ENTRY(nvm_ftl)     entry;
};

struct nvm_channel {
    uint16_t                    ch_id;
    uint16_t                    ch_mmgr_id;
    uint64_t                    ns_pgs;
    uint64_t                    slba;
    uint64_t                    elba;
    uint64_t                    tot_bytes;
    uint16_t                    mmgr_rsv; /* number of blks reserved by mmgr */
    uint16_t                    ftl_rsv;  /* number of blks reserved by ftl */
    struct nvm_mmgr             *mmgr;
    struct nvm_ftl              *ftl;
    struct nvm_mmgr_geometry    *geometry;
    struct nvm_ppa_addr         *mmgr_rsv_list; /* list of mmgr reserved blks */
    struct nvm_ppa_addr         *ftl_rsv_list;
    LIST_ENTRY(nvm_channel)     entry;
    union {
        struct {
            uint64_t   ns_id         :16;
            uint64_t   ns_part       :32;
            uint64_t   ftl_id        :8;
            uint64_t   in_use        :8;
        } i;
        uint64_t       nvm_info;
    };
};

#define CMDARG_LEN          32
#define CMDARG_FLAG_A       (1 << 0)
#define CMDARG_FLAG_S       (1 << 1)
#define CMDARG_FLAG_T       (1 << 2)
#define CMDARG_FLAG_L       (1 << 3)

#define OX_RUN_MODE         0x0
#define OX_TEST_MODE        0x1
#define OX_ADMIN_MODE       0x2

struct nvm_init_arg
{
    /* GLOBAL */
    int         cmdtype;
    int         arg_num;
    uint32_t    arg_flag;
    /* CMD TEST */
    char        test_setname[CMDARG_LEN];
    char        test_subtest[CMDARG_LEN];
    /* CMD ADMIN */
    char        admin_task[CMDARG_LEN];
};

/* tests initialization functions */
typedef int (tests_init_fn)(struct nvm_init_arg *);
typedef void *(tests_start_fn)(void *);
typedef void *(tests_admin_fn)(void *);
typedef void (tests_complete_io_fn)(struct NvmeRequest *);

struct tests_init_st {
    tests_init_fn           *init;
    tests_start_fn          *start;
    tests_admin_fn          *admin;
    tests_complete_io_fn    *complete_io;
};

struct nvm_sync_io_arg {
    uint8_t                 cmdtype;
    struct nvm_channel      *ch;
    struct nvm_mmgr_io_cmd  *cmd;
    void                    *buf;
    uint8_t                 status;
};

typedef struct QemuOxCtrl {
    PCIDevice       parent_obj;
    PCIDevice       *pci_dev;
    MemoryRegion    iomem;
    MemoryRegion    ctrl_mem;
    BlockConf       conf;
    uint32_t        reg_size;
    uint32_t        num_queues;
    char            *serial;
} QemuOxCtrl;

struct core_struct {
    uint8_t                 mmgr_count;
    uint16_t                ftl_count;
    uint16_t                ftl_q_count;
    uint16_t                nvm_ch_count;
    uint64_t                nvm_ns_size;
    jmp_buf                 jump;
    uint8_t                 run_flag;
    uint8_t                 debug;
    struct nvm_pcie         *nvm_pcie;
    struct nvm_channel      **nvm_ch;
    struct NvmeCtrl         *nvm_nvme_ctrl;
    struct tests_init_st    *tests_init;
    struct nvm_init_arg     *args_global;
    QemuOxCtrl              *qemu;
};

/* core functions */
int  nvm_register_mmgr(struct nvm_mmgr *);
int  nvm_register_pcie_handler(struct nvm_pcie *);
int  nvm_register_ftl (struct nvm_ftl *);
int  nvm_submit_io (struct nvm_io_cmd *);
int  nvm_submit_mmgr_io (struct nvm_mmgr_io_cmd *);
void nvm_callback (struct nvm_mmgr_io_cmd *);
void nvm_complete_io (struct nvm_io_cmd *);
int  nvm_dma (void *, uint64_t, ssize_t, uint8_t);
int  nvm_memcheck (void *);
int  nvm_ftl_cap_exec (uint8_t, void **, int);
int  nvm_init_ctrl (int, char **, QemuOxCtrl *);
int  nvm_test_unit (struct nvm_init_arg *);
int  nvm_admin_unit (struct nvm_init_arg *);
int  nvm_submit_sync_io (struct nvm_channel *, struct nvm_mmgr_io_cmd *,
                                                              void *, uint8_t);
int  nvm_submit_multi_plane_sync_io (struct nvm_channel *,
                          struct nvm_mmgr_io_cmd *, void *, uint8_t, uint64_t);

/* media managers init function */
int mmgr_dfcnand_init(void);
int mmgr_volt_init(void);

/* FTLs init function */
int ftl_lnvm_init(void);

/* nvme functions */
int  nvme_init(struct NvmeCtrl *);
void nvme_process_reg (struct NvmeCtrl *, uint64_t, uint64_t);
void nvme_q_scheduler (struct NvmeCtrl *, uint32_t *);
void nvme_process_db (struct NvmeCtrl *, uint64_t, uint64_t);
/* nvme functions used by tests */
uint16_t nvme_admin_cmd (struct NvmeCtrl *, struct NvmeCmd *,
                                                        struct NvmeRequest *);

/* pcie handler init function */
int dfcpcie_init(void);

/* console command parser init function */
int cmdarg_init (int, char **);

#endif /* SSD_H */