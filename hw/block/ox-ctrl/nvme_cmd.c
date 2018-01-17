#include <stdint.h>
#include <sys/queue.h>
#include <syslog.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <string.h>
#include <pthread.h>
#include "hw/block/ox-ctrl/include/lightnvm.h"
#include "hw/block/ox-ctrl/include/nvme.h"

extern struct core_struct core;

static void nvme_debug_print_io (NvmeRwCmd *cmd, uint32_t bs, uint64_t dt_sz,
        uint64_t md_sz, uint64_t elba, uint64_t *prp)
{
    int i;
    char buf[4096];

    setbuffer (stdout, buf, 4096);

    printf("  fuse: %d, psdt: %d\n", cmd->fuse, cmd->psdt);
    printf("  number of LBAs: %d, bs: %d\n", cmd->nlb + 1, bs);
    printf("  DMA size: %lu (data) + %lu (meta) = %lu bytes\n",
                                                 dt_sz, md_sz, dt_sz + md_sz);
    printf("  starting LBA: %lu, ending LBA: %lu\n", cmd->slba, elba);
    printf("  meta_prp: 0x%016lx\n", cmd->mptr);

    for (i = 0; i < cmd->nlb + 1; i++)
        printf("  [prp(%d): 0x%016lx\n", i, prp[i]);

    fflush (stdout);
    setlinebuf (stdout);
}

uint16_t nvme_identify (NvmeCtrl *n, NvmeCmd *cmd)
{
    NvmeNamespace *ns;
    NvmeIdentify *c = (NvmeIdentify *)cmd;
    NvmeIdCtrl *id = &n->id_ctrl;
    uint32_t cns  = c->cns;
    uint32_t nsid = c->nsid;
    uint64_t prp1 = c->prp1;
    uint32_t ns_list[1024];

    switch (cns) {
        case 1:
            if (prp1)
                return nvme_write_to_host(id, prp1, sizeof (NvmeIdCtrl));
            break;
        case 0:
            if (nsid == 0 || nsid > n->num_namespaces)
                return NVME_INVALID_NSID | NVME_DNR;

            ns = &n->namespaces[nsid - 1];
            if (prp1)
                return nvme_write_to_host(&ns->id_ns, prp1, sizeof(NvmeIdNs));
            break;
        case 2:

            if (nsid == 0xfffffffe || nsid == 0xffffffff)
                return NVME_INVALID_NSID | NVME_DNR;

            /* For now, only 1 valid namespace */
            if (nsid == 0)
                ns_list[0] = 0x1;

            if (prp1)
                return nvme_write_to_host(&ns_list, prp1, 4096);
            break;
        default:
            return NVME_INVALID_NSID | NVME_DNR;
    }

    return NVME_INVALID_NSID | NVME_DNR;
}

uint16_t nvme_del_sq (NvmeCtrl *n, NvmeCmd *cmd)
{
    NvmeDeleteQ *c = (NvmeDeleteQ *)cmd;
    NvmeRequest *req;
    NvmeSQ *sq;
    NvmeCQ *cq;
    uint16_t qid = c->qid;

    if (!qid || nvme_check_sqid (n, qid)) {
	return NVME_INVALID_QID | NVME_DNR;
    }

    sq = n->sq[qid];
    TAILQ_FOREACH (req, &sq->out_req_list, entry) {
    	// TODO: handle out_req_list
    }
    if (!nvme_check_cqid (n, sq->cqid)) {
    	cq = n->cq[sq->cqid];
	pthread_mutex_lock(&n->req_mutex);
	TAILQ_REMOVE (&cq->sq_list, sq, entry);
	pthread_mutex_unlock(&n->req_mutex);

	nvme_post_cqes (cq);
	pthread_mutex_lock(&n->req_mutex);
        TAILQ_FOREACH (req, &cq->req_list, entry) {
            if (req->sq == sq) {
                TAILQ_REMOVE (&cq->req_list, req, entry);
                TAILQ_INSERT_TAIL (&sq->req_list, req, entry);
                if (cq->hold_sqs) cq->hold_sqs = 0;
            }
        }
        pthread_mutex_unlock(&n->req_mutex);
    }
    n->qsched.SQID[(qid - 1) >> 5] &= (~(1UL << ((qid - 1) & 31)));
    n->qsched.prio_avail[sq->prio] = n->qsched.prio_avail[sq->prio] - 1;
    n->qsched.mask_regs[sq->prio][(qid - 1) >> 6] &=
                                (~(1UL << ((qid - 1) & (SHADOW_REG_SZ - 1))));
    n->qsched.shadow_regs[sq->prio][(qid -1) >> 6] &=
                                (~(1UL << ((qid - 1) & (SHADOW_REG_SZ - 1))));
    n->qsched.n_active_iosqs--;

    nvme_free_sq (sq, n);
    return NVME_SUCCESS;
}

uint16_t nvme_create_sq (NvmeCtrl *n, NvmeCmd *cmd)
{
    NvmeSQ *sq;
    NvmeCreateSq *c = (NvmeCreateSq *)cmd;

    uint16_t cqid = le16_to_cpu(c->cqid);
    uint16_t sqid = le16_to_cpu(c->sqid);
    uint16_t qsize = le16_to_cpu(c->qsize);
    uint16_t qflags = le16_to_cpu(c->sq_flags);
    uint64_t prp1 = le64_to_cpu(c->prp1);

    if (!cqid || nvme_check_cqid(n, cqid)) {
        return NVME_INVALID_CQID | NVME_DNR;
    }
    if (!sqid || (sqid && !nvme_check_sqid(n, sqid))) {
        return NVME_INVALID_QID | NVME_DNR;
    }
    if (!qsize || qsize > NVME_CAP_MQES(n->nvme_regs.vBar.cap)) {
        return NVME_MAX_QSIZE_EXCEEDED | NVME_DNR;
    }
    if (!prp1 || prp1 & (n->page_size - 1)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (!(NVME_SQ_FLAGS_PC(qflags)) && NVME_CAP_CQR(n->nvme_regs.vBar.cap)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    sq = g_malloc0(sizeof(*sq));
    if (nvme_init_sq(sq, n, prp1, sqid, cqid, qsize + 1,
            NVME_SQ_FLAGS_QPRIO(qflags),
            NVME_SQ_FLAGS_PC(qflags))) {
        g_free(sq);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    return NVME_SUCCESS;
}

uint16_t nvme_del_cq (NvmeCtrl *n, NvmeCmd *cmd)
{
    NvmeDeleteQ *c = (NvmeDeleteQ *)cmd;
    NvmeCQ *cq;
    uint16_t qid = c->qid;

    if (!qid || nvme_check_cqid (n, qid)) {
        return NVME_INVALID_CQID | NVME_DNR;
    }

    cq = n->cq[qid];
    if (!TAILQ_EMPTY (&cq->sq_list)) {
	return NVME_INVALID_QUEUE_DEL;
    }
    nvme_free_cq (cq, n);
    return NVME_SUCCESS;
}

uint16_t nvme_create_cq (NvmeCtrl *n, NvmeCmd *cmd)
{
    NvmeCQ *cq;
    NvmeCreateCq *c = (NvmeCreateCq *)cmd;
    uint16_t cqid = le16_to_cpu(c->cqid);
    uint16_t vector = le16_to_cpu(c->irq_vector);
    uint16_t qsize = le16_to_cpu(c->qsize);
    uint16_t qflags = le16_to_cpu(c->cq_flags);
    uint64_t prp1 = le64_to_cpu(c->prp1);

    if (!cqid || (cqid && !nvme_check_cqid(n, cqid))) {
        return NVME_INVALID_CQID | NVME_DNR;
    }
    if (!qsize || qsize > NVME_CAP_MQES(n->nvme_regs.vBar.cap)) {
        return NVME_MAX_QSIZE_EXCEEDED | NVME_DNR;
    }
    if (!prp1) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (vector > n->num_queues) {
        return NVME_INVALID_IRQ_VECTOR | NVME_DNR;
    }
    if (!(NVME_CQ_FLAGS_PC(qflags)) && NVME_CAP_CQR(n->nvme_regs.vBar.cap)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    cq = g_malloc0(sizeof(*cq));
    if (nvme_init_cq(cq, n, prp1, cqid, vector, qsize + 1,
            NVME_CQ_FLAGS_IEN(qflags), NVME_CQ_FLAGS_PC(qflags))) {
        g_free(cq);
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    return NVME_SUCCESS;
}

uint16_t nvme_set_feature (NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    uint32_t dw10 = cmd->cdw10;
    uint32_t dw11 = cmd->cdw11;
    uint32_t nsid = cmd->nsid;

    switch (dw10) {
        case NVME_ARBITRATION:
            req->cqe.n.result = htole32(n->features.arbitration);
            n->features.arbitration = dw11;
            break;
	case NVME_POWER_MANAGEMENT:
            n->features.power_mgmt = dw11;
            break;
	case NVME_LBA_RANGE_TYPE:
            if (nsid == 0 || nsid > n->num_namespaces) {
                return NVME_INVALID_NSID | NVME_DNR;
            }
            return NVME_SUCCESS;
	case NVME_NUMBER_OF_QUEUES:
            if ((dw11 < 1) || (dw11 > NVME_MAX_QUEUE_ENTRIES)) {
		req->cqe.n.result =
                            htole32 (n->num_queues | (n->num_queues << 16));
            } else {
		req->cqe.n.result = htole32(dw11 | (dw11 << 16));
		n->num_queues = dw11;
            }
            break;
	case NVME_TEMPERATURE_THRESHOLD:
            n->features.temp_thresh = dw11;
            if (n->features.temp_thresh <= n->temperature
                                                    && !n->temp_warn_issued) {
                n->temp_warn_issued = 1;
		nvme_enqueue_event (n, NVME_AER_TYPE_SMART,
                            		NVME_AER_INFO_SMART_TEMP_THRESH,
					NVME_LOG_SMART_INFO);
            } else if (n->features.temp_thresh > n->temperature &&
		!(n->aer_mask & 1 << NVME_AER_TYPE_SMART)) {
		n->temp_warn_issued = 0;
            }
            break;
        case NVME_ERROR_RECOVERY:
            n->features.err_rec = dw11;
            break;
	case NVME_VOLATILE_WRITE_CACHE:
            n->features.volatile_wc = dw11;
            break;
	case NVME_INTERRUPT_COALESCING:
            n->features.int_coalescing = dw11;
            break;
	case NVME_INTERRUPT_VECTOR_CONF:
            if ((dw11 & 0xffff) > n->num_queues) {
                return NVME_INVALID_FIELD | NVME_DNR;
            }
            n->features.int_vector_config[dw11 & 0xffff] = dw11 & 0x1ffff;
            break;
	case NVME_WRITE_ATOMICITY:
            n->features.write_atomicity = dw11;
            break;
	case NVME_ASYNCHRONOUS_EVENT_CONF:
            n->features.async_config = dw11;
            break;
	case NVME_SOFTWARE_PROGRESS_MARKER:
            n->features.sw_prog_marker = dw11;
            break;
	default:
            return NVME_INVALID_FIELD | NVME_DNR;
    }
    return NVME_SUCCESS;
}

uint16_t nvme_get_feature (NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    uint32_t dw10 = cmd->cdw10;
    uint32_t dw11 = cmd->cdw11;
    uint32_t nsid = cmd->nsid;

    switch (dw10) {
	case NVME_ARBITRATION:
            req->cqe.n.result = htole32(n->features.arbitration);
            break;
	case NVME_POWER_MANAGEMENT:
            req->cqe.n.result = htole32(n->features.power_mgmt);
            break;
	case NVME_LBA_RANGE_TYPE:
            if (nsid == 0 || nsid > n->num_namespaces) {
		return NVME_INVALID_NSID | NVME_DNR;
            }
            return NVME_SUCCESS;
	case NVME_NUMBER_OF_QUEUES:
            req->cqe.n.result = htole32(n->num_queues | (n->num_queues << 16));
            break;
        case NVME_TEMPERATURE_THRESHOLD:
            req->cqe.n.result = htole32(n->features.temp_thresh);
            break;
	case NVME_ERROR_RECOVERY:
            req->cqe.n.result = htole32(n->features.err_rec);
            break;
	case NVME_VOLATILE_WRITE_CACHE:
            req->cqe.n.result = htole32(n->features.volatile_wc);
            break;
	case NVME_INTERRUPT_COALESCING:
            req->cqe.n.result = htole32(n->features.int_coalescing);
            break;
	case NVME_INTERRUPT_VECTOR_CONF:
            if ((dw11 & 0xffff) > n->num_queues) {
                return NVME_INVALID_FIELD | NVME_DNR;
            }
            req->cqe.n.result = htole32(
            n->features.int_vector_config[dw11 & 0xffff]);
            break;
	case NVME_WRITE_ATOMICITY:
            req->cqe.n.result = htole32(n->features.write_atomicity);
            break;
        case NVME_ASYNCHRONOUS_EVENT_CONF:
            req->cqe.n.result = htole32(n->features.async_config);
            break;
        case NVME_SOFTWARE_PROGRESS_MARKER:
            req->cqe.n.result = htole32(n->features.sw_prog_marker);
            break;
	default:
            return NVME_INVALID_FIELD | NVME_DNR;
    }
    return NVME_SUCCESS;
}

static uint16_t nvme_error_log_info (NvmeCtrl *n, NvmeCmd *cmd)
{
    uint64_t prp1 = cmd->prp1;
    n->aer_mask &= ~(1 << NVME_AER_TYPE_ERROR);
    if(prp1)
        return nvme_write_to_host(n->elpes, prp1, sizeof (NvmeErrorLog));
    return NVME_SUCCESS;
}

static uint16_t nvme_smart_info (NvmeCtrl *n, NvmeCmd *cmd, uint32_t buf_len)
{
    uint64_t prp1 = cmd->prp1;
    time_t current_seconds;
    int Rtmp = 0,Wtmp = 0;
    int read = 0,write = 0;
    NvmeSmartLog smart;

    memset (&smart, 0x0, sizeof (smart));
    Rtmp = n->stat.nr_bytes_read/1000;
    Wtmp = n->stat.nr_bytes_written/1000;

    read = n->stat.nr_bytes_read%1000;
    write = n->stat.nr_bytes_written%1000;

    read = (read >= 500)?1:0;
    write = (write >=500)?1:0;

    n->stat.nr_bytes_read = Rtmp + read;
    n->stat.nr_bytes_written = Wtmp + write;

    smart.data_units_read[0] = htole64(n->stat.nr_bytes_read);
    smart.data_units_written[0] = htole64(n->stat.nr_bytes_written);
    smart.host_read_commands[0] = htole64(n->stat.tot_num_ReadCmd);
    smart.host_write_commands[0] = htole64(n->stat.tot_num_WriteCmd);
    smart.number_of_error_log_entries[0] = htole64(n->num_errors);
    smart.temperature[0] = n->temperature & 0xff;
    smart.temperature[1] = (n->temperature >> 8) & 0xff;

    current_seconds = time (NULL);
    smart.power_on_hours[0] = htole64(
                                ((current_seconds - n->start_time) / 60) / 60);

    smart.available_spare_threshold = NVME_SPARE_THRESHOLD;
    if (smart.available_spare <= NVME_SPARE_THRESHOLD) {
	smart.critical_warning |= NVME_SMART_SPARE;
    }
    if (n->features.temp_thresh <= n->temperature) {
	smart.critical_warning |= NVME_SMART_TEMPERATURE;
    }

    n->aer_mask &= ~(1 << NVME_AER_TYPE_SMART);
    NvmeSmartLog *smrt = &smart;
    if(prp1)
        return nvme_write_to_host(smrt, prp1, sizeof (NvmeSmartLog));
    return NVME_SUCCESS;
}

static inline uint16_t nvme_fw_log_info (NvmeCtrl *n, NvmeCmd *cmd,
                                                            uint32_t buf_len)
{
    //uint32_t trans_len;
    //uint64_t prp1 = cmd->prp1;
    //uint64_t prp2 = cmd->prp2;
    //NvmeFwSlotInfoLog fw_log;

    /* NOT IMPLEMENTED, TODO */
    return NVME_SUCCESS;
}

inline uint16_t nvme_get_log(NvmeCtrl *n, NvmeCmd *cmd)
{
    uint32_t dw10 = cmd->cdw10;
    uint16_t lid = dw10 & 0xffff;
    uint32_t len = ((dw10 >> 16) & 0xff) << 2;

    switch (lid) {
	case NVME_LOG_ERROR_INFO:
            return nvme_error_log_info (n, cmd);
	case NVME_LOG_SMART_INFO:
            return nvme_smart_info (n, cmd, len);
	case NVME_LOG_FW_SLOT_INFO:
            return nvme_fw_log_info (n, cmd, len);
	default:
            return NVME_INVALID_LOG_ID | NVME_DNR;
    }
}

uint16_t nvme_abort_req (NvmeCtrl *n, NvmeCmd *cmd, uint32_t *result)
{
 /* The Abort command is used to abort a specific command previously submitted
  * to the Admin Submission Queue or an I/O Submission Queue. An Abort command
  * is a best effort command; the command to abort may have already completed,
  * currently be in execution, or may be deeply queued. It is implementation
  * specific when a controller chooses to complete the Abort command when the
  * command to abort is not found. */
    return NVME_SUCCESS;
}

static uint16_t nvme_format_namespace (NvmeNamespace *ns, uint8_t lba_idx,
		uint8_t meta_loc, uint8_t pil, uint8_t pi, uint8_t sec_erase)
{
    uint64_t blks;
    uint16_t ms = ns->id_ns.lbaf[lba_idx].ms;

    if (lba_idx > ns->id_ns.nlbaf) {
	return NVME_INVALID_FORMAT | NVME_DNR;
    }
    if (pi) {
	if (pil && !NVME_ID_NS_DPC_LAST_EIGHT(ns->id_ns.dpc)) {
            return NVME_INVALID_FORMAT | NVME_DNR;
	}
	if (!pil && !NVME_ID_NS_DPC_FIRST_EIGHT(ns->id_ns.dpc)) {
            return NVME_INVALID_FORMAT | NVME_DNR;
	}
	if (!((ns->id_ns.dpc & 0x7) & (1 << (pi - 1)))) {
            return NVME_INVALID_FORMAT | NVME_DNR;
	}
    }
    if (meta_loc && ms && !NVME_ID_NS_MC_EXTENDED(ns->id_ns.mc)) {
	return NVME_INVALID_FORMAT | NVME_DNR;
    }
    if (!meta_loc && ms && !NVME_ID_NS_MC_SEPARATE(ns->id_ns.mc)) {
    	return NVME_INVALID_FORMAT | NVME_DNR;
    }

    FREE_VALID (ns->util);
    FREE_VALID (ns->uncorrectable);
    blks = ns->ctrl->ns_size[ns->id - 1] / ((1 << ns->id_ns.lbaf[lba_idx].ds)
                                                            + ns->ctrl->meta);
    ns->id_ns.flbas = lba_idx | meta_loc;
    ns->id_ns.nsze = htole64(blks);
    ns->id_ns.ncap = ns->id_ns.nsze;
    ns->id_ns.nuse = ns->id_ns.nsze;
    ns->id_ns.dps = pil | pi;

/* TODO: restart lnvm tbls */
/*
    if (lightnvm_dev(n)) {
        ns->tbl_dsk_start_offset = ns->start_block;
        ns->tbl_entries = blks;
        if (ns->tbl) {
            free(ns->tbl);
        }
        ns->tbl = calloc(1, lightnvm_tbl_size(ns));
        lightnvm_tbl_initialize(ns);
    } else {
        ns->tbl = NULL;
        ns->tbl_entries = 0;
    }
 */

    if (sec_erase) {
	/* TODO: write zeros, complete asynchronously */;
    }

    return NVME_SUCCESS;
}

uint16_t nvme_format (NvmeCtrl *n, NvmeCmd *cmd)
{
    /* TODO: Not completely implemented */

    NvmeNamespace *ns;
    uint32_t dw10 = cmd->cdw10;
    uint32_t nsid = cmd->nsid;

    uint8_t lba_idx = dw10 & 0xf;
    uint8_t meta_loc = dw10 & 0x10;
    uint8_t pil = (dw10 >> 5) & 0x8;
    uint8_t pi = (dw10 >> 5) & 0x7;
    uint8_t sec_erase = (dw10 >> 8) & 0x7;

    if (nsid == 0xffffffff) {
	uint32_t i;
	uint16_t ret = NVME_SUCCESS;

    	for (i = 0; i < n->num_namespaces; ++i) {
            ns = &n->namespaces[i];
            ret = nvme_format_namespace (ns,lba_idx,meta_loc,pil,pi,sec_erase);
            if (ret != NVME_SUCCESS) {
		return ret;
            }
	}
	return ret;
    }

    if (nsid == 0 || nsid > n->num_namespaces)
        return NVME_INVALID_NSID | NVME_DNR;

    ns = &n->namespaces[nsid - 1];
    return nvme_format_namespace (ns, lba_idx, meta_loc, pil, pi, sec_erase);
}

uint16_t nvme_async_req (NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
/*
Asynchronous events are used to notify host software of status, error, and
health information as these events occur. To enable asynchronous events to be
reported by the controller, host software needs to submit one or more
Asynchronous Event Request commands to the controller. The controller specifies
an event to the host by completing an Asynchronous Event Request command. Host
software should expect that the controller may not execute the command
immediately; the command should be completed when there is an event to be
reported. The Asynchronous Event Request command is submitted by host software
to enable the reporting of asynchronous events from the controller. This
command has no timeout. The controller posts a completion queue entry for this
command when there is an asynchronous event to report to the host. If
Asynchronous Event Request commands are outstanding when the controller is
reset, the commands are aborted.
*/
    /* TODO: Not implemented yet */

    return NVME_NO_COMPLETE;
}

uint16_t nvme_write_uncor(NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
		NvmeRequest *req)
{
/*
     The Write Uncorrectable command is used to mark a range of logical blocks
     as invalid. When the specified logical block(s) are read after this
     operation, a failure is returned with Unrecovered Read Error status. To
     clear the invalid logical block status, a write operation is performed on
     those logical blocks. The fields used are Command Dword 1
*/
    /* TODO: Not implemented yet */

    return NVME_SUCCESS;
}

uint16_t nvme_dsm (NvmeCtrl *n,NvmeNamespace *ns,NvmeCmd *cmd,NvmeRequest *req)
{
/*
 The Dataset Management command is used by the host to indicate attributes for
 ranges of logical blocks. This includes attributes like frequency that data
 is read or written, access size, and other information that may be used to
 optimize performance and reliability. This command is advisory; a compliant
 controller may choose to take no action based on information provided.
 The command uses Command Dword 10, and Command Dword 11 fields. If the command
 uses PRPs for the data transfer, then the PRP Entry 1 and PRP Entry 2 fields
 are used. If the command uses SGLs for the data transfer, then the SGL Entry 1
 field is used. All other command specific fields are reserved.
 */
    /* TODO: Not implemented yet */

    return NVME_SUCCESS;
}

uint16_t nvme_flush(NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
		NvmeRequest *req)
{
/*
 In case of volatile write cache is enable, flush is used to store the current
 data in the cash to non-volatile memory.
 */
    /* TODO: Not Implemented yet */

    return NVME_SUCCESS;
}

uint16_t nvme_compare(NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
    NvmeRequest *req)
{
/*
The Compare command reads the logical blocks specified by the command from the
medium and compares the data read to a comparison data buffer transferred as
part of the command. If the data read from the controller and the comparison
data buffer are equivalent with no miscompares, then the command completes
successfully. If there is any miscompare, the command completes with an error
of Compare Failure. If metadata is provided, then a comparison is also performed
for the metadata, excluding protection information. Refer to section 8.3. The
command uses Command Dword 10, Command Dword 11, Command Dword 12, Command
Dword 14, and Command Dword 15 fields. If the command uses PRPs for the data
transfer, then the Metadata Pointer, PRP Entry 1, and PRP Entry 2 fields are
used. If the command uses SGLs for the data transfer, then the Metadata SGL
Segment Pointer and SGL Entry 1 fields are used. All other command specific
fields are reserved.
*/
    /* TODO: Not implemented yet */

    return NVME_SUCCESS;
}

uint16_t nvme_write_zeros(NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
    NvmeRequest *req)
{
/*
The Write Zeroes command is used to set a range of logical blocks to zero.
After successful completion of this command, the value returned by subsequent
reads of logical blocks in this range shall be zeroes until a write occurs to
this LBA range. The metadata for this command shall be all zeroes and the
protection information is updated based on CDW12.PRINFO. The fields used are
Command Dword 10, Command Dword 11, Command Dword 12, Command Dword 14, and
Command Dword 15 fields
*/
    /* TODO: Not implemented yet */

    return NVME_SUCCESS;
}

uint16_t nvme_rw (NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                                                             NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    int i;

    uint32_t nlb  = rw->nlb + 1;
    uint64_t slba = rw->slba;
    uint64_t prp1 = rw->prp1;
    uint64_t prp2 = rw->prp2;

    const uint64_t elba = slba + nlb;
    const uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    const uint8_t data_shift = ns->id_ns.lbaf[lba_index].ds;
    uint64_t data_size = nlb << data_shift;

    req->nvm_io.status.status = NVM_IO_NEW;

    req->is_write = rw->opcode == NVME_CMD_WRITE;

    if (elba > (ns->id_ns.nsze))
	return NVME_LBA_RANGE | NVME_DNR;

    if (n->id_ctrl.mdts && data_size > n->page_size * (1 << n->id_ctrl.mdts))
	return NVME_LBA_RANGE | NVME_DNR;

    if (nlb > 256)
	return NVME_INVALID_FIELD | NVME_DNR;

    /* Metadata disabled
    const uint16_t ms = ns->id_ns.lbaf[lba_index].ms;
    uint64_t meta_size = nlb * ms;
    if (meta_size)
        return NVME_INVALID_FIELD | NVME_DNR;
    */

    /* End-to-end Data protection disabled
    if ((ctrl & NVME_RW_PRINFO_PRACT) && !(ns->id_ns.dps & DPS_TYPE_MASK))
        return NVME_INVALID_FIELD | NVME_DNR;
    */

    /* TODO: Map PRPs for SGL addresses */
    switch (rw->psdt) {
        case CMD_PSDT_PRP:
        case CMD_PSDT_RSV:
            req->nvm_io.prp[0] = prp1;

            if (nlb == 2)
                req->nvm_io.prp[1] = prp2;
            else if (nlb > 2)
                nvme_read_from_host((void *)(&req->nvm_io.prp[1]), prp2,
                                                 (nlb - 1) * sizeof(uint64_t));
            break;
        case CMD_PSDT_SGL:
        case CMD_PSDT_SGL_MD:
            return NVME_INVALID_FORMAT;
    }

    req->slba = slba;
    req->meta_size = 0;
    req->status = NVME_SUCCESS;
    req->nlb = nlb;
    req->ns = ns;
    req->lba_index = lba_index;

    req->nvm_io.cid = rw->cid;
    req->nvm_io.sec_sz = NVME_KERNEL_PG_SIZE;
    req->nvm_io.md_sz = 0;
    req->nvm_io.cmdtype = (req->is_write) ? MMGR_WRITE_PG : MMGR_READ_PG;
    req->nvm_io.n_sec = nlb;
    req->nvm_io.req = (void *) req;
    req->nvm_io.slba = slba;

    req->nvm_io.status.pg_errors = 0;
    req->nvm_io.status.ret_t = 0;
    req->nvm_io.status.total_pgs = 0;
    req->nvm_io.status.pgs_p = 0;
    req->nvm_io.status.pgs_s = 0;
    req->nvm_io.status.status = NVM_IO_NEW;

    for (i = 0; i < 8; i++) {
        req->nvm_io.status.pg_map[i] = 0;
    }

    if (core.debug)
        nvme_debug_print_io (rw, req->nvm_io.sec_sz, data_size,
                                     req->nvm_io.md_sz, elba, req->nvm_io.prp);

    return nvm_submit_ftl(&req->nvm_io);
}