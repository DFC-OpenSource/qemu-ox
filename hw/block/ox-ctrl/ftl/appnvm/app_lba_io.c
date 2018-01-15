/* OX: Open-Channel NVM Express SSD Controller
 *  - AppNVM Flash Translation Layer (Logical Block Address I/O)
 *
 * Copyright 2018 IT University of Copenhagen.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 *
 * Partially supported by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 */

#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/queue.h>
#include "hw/block/ox-ctrl/include/ssd.h"
#include "appnvm.h"

static void lba_io_callback (struct nvm_io_cmd *cmd)
{

}

static int lba_io_submit (struct nvm_io_cmd *cmd)
{
    return 0;
}

static void lba_io_exit (void)
{

}

static int lba_io_init (void)
{
    return 0;
}

void lba_io_register (void) {
    appnvm()->lba_io.init_fn     = lba_io_init;
    appnvm()->lba_io.exit_fn     = lba_io_exit;
    appnvm()->lba_io.submit_fn   = lba_io_submit;
    appnvm()->lba_io.callback_fn = lba_io_callback;
}