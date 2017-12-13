#ifndef OX_NDP_H
#define OX_NDP_H

enum NdpAdminCommands {
    NDP_ADM_CMD_INFO       = 0xe6,
    NDP_ADM_CMD_INST_DAEM  = 0xd1,
    NDP_ADM_CMD_DEL_DAEM   = 0xd0
};

enum NdpExecCommands {
    NDP_EXEC_RUN_JOB       = 0xa1,
    NDP_EXEC_DAEM_REQ      = 0xa3
};

#endif /* OX_NDP_H */

