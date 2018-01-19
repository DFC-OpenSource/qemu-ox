/* OX: Open-Channel NVM Express SSD Controller
 *  - AppNVM Garbage Collecting
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

#include <stdlib.h>
#include <stdio.h>
#include "appnvm.h"
#include <sys/queue.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include "hw/block/ox-ctrl/include/ssd.h"

#define APP_GC_PARALLEL_CH   3
#define APP_GC_CHECK_US      10000

extern uint16_t             app_nch;
static struct app_channel **ch;
static pthread_t            check_th;
static uint8_t              stop;

static void *gc_run_ch (void *arg)
{
    struct app_channel *lch = (struct app_channel *) arg;

    printf ("CH %d has started GC!\n", lch->app_ch_id);
    sleep (5);
    appnvm_ch_need_gc_unset (lch);
    printf ("CH %d has finished GC!\n", lch->app_ch_id);
    
    return NULL;
}

static void *gc_check_fn (void *arg)
{
    uint16_t cch = 0, sleep = 0, n_run = 0;
    pthread_t run_th[app_nch];
    
    memset (run_th, 0x0, sizeof (pthread_t) * app_nch);

    while (!stop) {
        
        if (appnvm_ch_need_gc (ch[cch])) {            
            if (pthread_create (&run_th[cch], NULL, gc_run_ch, (void *) ch[cch]))
                continue;
            n_run++;
            sleep++;

            if (n_run == APP_GC_PARALLEL_CH || cch == app_nch - 1) {
                while (n_run) {
                    n_run--;
                    pthread_join (run_th[n_run], NULL);                    
                }
            }
        }
        
        cch = (cch == app_nch - 1) ? 0 : cch + 1;
        if (!cch) {            
            if (!sleep)
                usleep (APP_GC_CHECK_US);
            sleep = 0;
        }
    }

    return NULL;
}

static int gc_init (void)
{
    uint16_t nch;

    ch = malloc (sizeof (struct app_channel *) * app_nch);
    if (!ch)
        return -1;

    nch = appnvm()->channels.get_list_fn (ch, app_nch);
    if (nch != app_nch)
        goto FREE_CH;

    stop = 0;
    if (pthread_create (&check_th, NULL, gc_check_fn, NULL))
        goto FREE_CH;

    return 0;

FREE_CH:
    free (ch);
    return -1;
}

static void gc_exit (void)
{
    stop++;
    pthread_join (check_th, NULL);
    free (ch);
}

void gc_register (void)
{
    appnvm()->gc.init_fn = gc_init;
    appnvm()->gc.exit_fn = gc_exit;
}