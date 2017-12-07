#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <syslog.h>
#include <string.h>
#include <mqueue.h>
#include <sched.h>
#include "hw/block/ox-ctrl/include/ssd.h"
#include "hw/block/ox-ctrl/include/nvme.h"
#include "pcie_dfc.h"
#include "hw/pci/msix.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci.h"
#include "qemu/host-utils.h"

extern struct core_struct core;

static void *dfcpcie_req_processor (void *arg)
{
    return NULL;
}

static uint64_t ox_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    NvmeCtrl *n = core.nvm_nvme_ctrl;
    uint8_t *ptr = (uint8_t *)&n->nvme_regs.vBar;
    uint64_t val = 0;

    if (addr < sizeof(n->nvme_regs.vBar)) {
        memcpy(&val, ptr + addr, size);
    }
    return val;
}

static void ox_mmio_write(void *opaque, hwaddr addr, uint64_t data,
    unsigned size)
{
    NvmeCtrl *n = core.nvm_nvme_ctrl;

    if (addr < sizeof(n->nvme_regs.vBar)) {
        nvme_process_reg(n, addr, data);
    } else if (addr >= 0x1000) {
        nvme_process_db(n, addr, data);
    }
}

static const MemoryRegionOps ox_mmio_ops = {
    .read = ox_mmio_read,
    .write = ox_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 2,
        .max_access_size = 8,
    },
};

static void ox_cmb_write(void *opaque, hwaddr addr, uint64_t data,
    unsigned size)
{
    NvmeCtrl *n = core.nvm_nvme_ctrl;
    memcpy(&n->cmbuf[addr], &data, size);
}

static uint64_t ox_cmb_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t val;
    NvmeCtrl *n = core.nvm_nvme_ctrl;

    memcpy(&val, &n->cmbuf[addr], size);
    return val;
}

static const MemoryRegionOps ox_cmb_ops = {
    .read = ox_cmb_read,
    .write = ox_cmb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 2,
        .max_access_size = 8,
    },
};

static int pcie_init_pci (struct pci_ctrl *ctrl)
{
    PCIDevice *pci_dev = core.qemu->pci_dev;
    uint8_t *pci_conf;

    core.nvm_nvme_ctrl->reg_size = 1 << (32 - clz32(0x1004 +
                                2 * (core.nvm_nvme_ctrl->num_queues + 1) * 4));

    pci_conf = pci_dev->config;
    pci_conf[PCI_INTERRUPT_PIN] = 1;
    pci_config_set_prog_interface(pci_dev->config, 0x2);
    pci_config_set_vendor_id(pci_conf, PCI_VENDOR_ID_LNVM);
    pci_config_set_device_id(pci_conf, PCI_DEVICE_ID_LNVM);
    pci_config_set_class(pci_dev->config, PCI_CLASS_STORAGE_EXPRESS);
    pcie_endpoint_cap_init(&core.qemu->parent_obj, 0x80);

    memory_region_init_io(&core.qemu->iomem, OBJECT(core.qemu),
            &ox_mmio_ops, core.qemu, "ox-ctrl", core.nvm_nvme_ctrl->reg_size);

    pci_register_bar(&core.qemu->parent_obj, 0, PCI_BASE_ADDRESS_SPACE_MEMORY |
                            PCI_BASE_ADDRESS_MEM_TYPE_64, &core.qemu->iomem);

    msix_init_exclusive_bar(&core.qemu->parent_obj,
                                            core.nvm_nvme_ctrl->num_queues, 4);

    msi_init(&core.qemu->parent_obj, 0x50, 32, true, false, NULL);

    if (core.nvm_nvme_ctrl->cmbsz) {
        core.nvm_nvme_ctrl->nvme_regs.vBar.cmbloc = core.nvm_nvme_ctrl->cmbloc;
        core.nvm_nvme_ctrl->nvme_regs.vBar.cmbsz  = core.nvm_nvme_ctrl->cmbsz;

        core.nvm_nvme_ctrl->cmbuf = g_malloc0(NVME_CMBSZ_GETSIZE
                                    (core.nvm_nvme_ctrl->nvme_regs.vBar.cmbsz));

        memory_region_init_io(&core.qemu->ctrl_mem, OBJECT(core.qemu),
                        &ox_cmb_ops, core.qemu, "ox-cmb", NVME_CMBSZ_GETSIZE
                        (core.nvm_nvme_ctrl->nvme_regs.vBar.cmbsz));

        pci_register_bar(&core.qemu->parent_obj, NVME_CMBLOC_BIR
                (core.nvm_nvme_ctrl->nvme_regs.vBar.cmbloc),
                PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64,
                &core.qemu->ctrl_mem);
    }

    return 0;
}

struct nvm_pcie pcie_dfc = {
    .name           = "PCI_LS2085",
};

static void dfcpcie_isr_notify (void *opaque)
{
    NvmeCQ *cq = opaque;

    if (cq->irq_enabled) {
        if (msix_enabled(&(core.qemu->parent_obj))) {

            msix_notify(&(core.qemu->parent_obj), cq->vector);

        } else if (msi_enabled(&(core.qemu->parent_obj))) {

            if (!(core.nvm_nvme_ctrl->nvme_regs.vBar.intms & (1<<cq->vector))){
                msi_notify(&(core.qemu->parent_obj), cq->vector);
            }

        } else {
            pci_irq_pulse(&core.qemu->parent_obj);
        }
    }
}

static void dfcpcie_reset (void)
{
    return;
}

static void dfcpcie_exit(void) {
    struct pci_ctrl *pcie = (struct pci_ctrl *) pcie_dfc.ctrl;
    msix_uninit_exclusive_bar(core.qemu->pci_dev);
    memory_region_unref(&core.qemu->iomem);
    free(pcie);
}

struct nvm_pcie_ops pcidfc_ops = {
    .nvme_consumer      = dfcpcie_req_processor,
    .exit               = dfcpcie_exit,
    .isr_notify         = dfcpcie_isr_notify,
    .reset              = dfcpcie_reset
};

int dfcpcie_init(void)
{
    pcie_dfc.ctrl = (void *)calloc(sizeof(struct pci_ctrl), 1);

    if (!pcie_dfc.ctrl)
        return EMEM;

    if(pcie_init_pci(pcie_dfc.ctrl))
        return EPCIE_REGISTER;

    pcie_dfc.ops = &pcidfc_ops;

    return nvm_register_pcie_handler(&pcie_dfc);
}
