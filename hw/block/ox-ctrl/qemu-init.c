#include <string.h>
#include "qemu/osdep.h"
#include "hw/block/block.h"
#include "hw/hw.h"
#include "hw/pci/msix.h"
#include "hw/pci/pci.h"
#include "sysemu/sysemu.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "sysemu/block-backend.h"
#include "hw/block/ox-ctrl/include/ssd.h"

QemuOxCtrl *qemuOxCtrl;

static int ox_init(PCIDevice *pci_dev)
{
    int argc = 2;
    char **argv = malloc (sizeof(char *) * argc);
    argv[0] = malloc (8);
    argv[1] = malloc (6);
    memcpy(argv[0], "ox-ctrl\0", 8);
    memcpy(argv[1], qemuOxCtrl->mode, 5);
    argv[1][5] = '\0';

    blkconf_serial(&qemuOxCtrl->conf, &qemuOxCtrl->serial);

    qemuOxCtrl->pci_dev = pci_dev;
    nvm_init_ctrl (argc, argv, qemuOxCtrl);

    return 0;
}

static void ox_exit(PCIDevice *pci_dev)
{

}

static Property ox_props[] = {
    DEFINE_BLOCK_PROPERTIES(QemuOxCtrl, conf),
    DEFINE_PROP_STRING("serial", QemuOxCtrl, serial),
    DEFINE_PROP_STRING("mode", QemuOxCtrl, mode),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription ox_vmstate = {
    .name = "ox-ctrl",
    .unmigratable = 1,
};

static void ox_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->init = ox_init;
    pc->exit = ox_exit;
    pc->class_id = PCI_CLASS_STORAGE_EXPRESS;
    pc->vendor_id = PCI_VENDOR_ID_LNVM;
    pc->device_id = PCI_DEVICE_ID_LNVM;
    pc->revision = 2;
    pc->is_express = 1;

    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    dc->desc = "Ox-Ctrl - Open-Channel SSD Controller";
    dc->props = ox_props;
    dc->vmsd = &ox_vmstate;
}

static void ox_get_bootindex(Object *obj, Visitor *v,
                                  const char *name, void *opaque, Error **errp)
{
    QemuOxCtrl *qemu = OXCTRL(obj);

    visit_type_int32(v, name, &qemu->conf.bootindex, errp);
}

static void ox_set_bootindex(Object *obj, Visitor *v,
                                  const char *name, void *opaque, Error **errp)
{
    QemuOxCtrl *qemu = OXCTRL(obj);
    int32_t boot_index;
    Error *local_err = NULL;

    visit_type_int32(v, name, &boot_index, &local_err);
    if (local_err) {
        goto out;
    }
    /* check whether bootindex is present in fw_boot_order list  */
    check_boot_index(boot_index, &local_err);
    if (local_err) {
        goto out;
    }
    /* change bootindex to a new one */
    qemu->conf.bootindex = boot_index;

out:
    if (local_err) {
        error_propagate(errp, local_err);
    }
}

static void ox_instance_init(Object *obj)
{
    qemuOxCtrl = OXCTRL(obj);

    object_property_add(obj, "bootindex", "int32",
                        ox_get_bootindex,
                        ox_set_bootindex, NULL, NULL, NULL);
    object_property_set_int(obj, -1, "bootindex", NULL);
}

static const TypeInfo ox_info = {
    .name          = "ox-ctrl",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(QemuOxCtrl),
    .class_init    = ox_class_init,
    .instance_init = ox_instance_init,
};

static void ox_register_types(void)
{
    type_register_static(&ox_info);
}

type_init(ox_register_types);