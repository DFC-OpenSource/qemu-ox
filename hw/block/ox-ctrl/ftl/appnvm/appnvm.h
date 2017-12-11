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

#ifndef APPNVM_H
#define APPNVM_H

#include <sys/queue.h>
#include "hw/block/ox-ctrl/include/ssd.h"

#define APP_IO_RETRY       0
#define APP_RSV_BLK        1
#define APP_RSV_BLK_COUNT  1
#define APP_MAGIC          0x3c

enum {
    FTL_PGMAP_OFF   = 0,
    FTL_PGMAP_ON    = 1
};

enum app_bbt_state {
    NVM_BBT_FREE = 0x0, // Block is free AKA good
    NVM_BBT_BAD  = 0x1, // Block is bad
    NVM_BBT_GBAD = 0x2, // Block has grown bad
    NVM_BBT_DMRK = 0x4, // Block has been marked by device side
    NVM_BBT_HMRK = 0x8  // Block has been marked by host side
};

#define APP_BBT_EMERGENCY   0x0 // Creates the bbt without erasing the channel
#define APP_BBT_ERASE       0x1 // Checks for bad blocks only erasing the block
#define APP_BBT_FULL        0x2 // Checks for bad blocks erasing the block,
                                 //   writing and reading all pages,
                                 //   and comparing the buffers

struct app_page {

};

struct app_bbtbl {
    uint8_t  magic;
    uint32_t bb_sz;
    uint32_t bb_count;
    /* This struct is stored on NVM up to this point, *tbl is not stored */
    uint8_t  *tbl;
};

struct app_channel {
    struct nvm_channel       *ch;
    struct app_bbtbl        *bbtbl;
    LIST_ENTRY(app_channel) entry;
};

int app_get_bbt_nvm (struct app_channel *, struct app_bbtbl *);
int app_bbt_create (struct app_channel *, struct app_bbtbl *, uint8_t);
int app_flush_bbt (struct app_channel *, struct app_bbtbl *);

#endif /* APP_H */