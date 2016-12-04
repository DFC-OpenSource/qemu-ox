#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/queue.h>
#include "include/ox-mq.h"
#include "include/ssd.h"

void ox_mq_show_stats (struct ox_mq *mq)
{
    int i;
    struct ox_mq_queue *q;

    for (i = 0; i < mq->config->n_queues; i++) {
        q = &mq->queues[i];
        log_info ("Q %d. SF: %d, SU: %d, SW: %d, CF: %d, CU: %d\n", i,
                u_atomic_read(&q->stats.sq_free),
                u_atomic_read(&q->stats.sq_used),
                u_atomic_read(&q->stats.sq_wait),
                u_atomic_read(&q->stats.cq_free),
                u_atomic_read(&q->stats.cq_used));
    }
}

static void ox_mq_destroy_sq (struct ox_mq_queue *q)
{
    pthread_mutex_destroy (&q->sq_free_mutex);
    pthread_mutex_destroy (&q->sq_used_mutex);
    pthread_mutex_destroy (&q->sq_wait_mutex);
    pthread_mutex_destroy (&q->sq_cond_m);
    pthread_cond_destroy (&q->sq_cond);
}

static void ox_mq_destroy_cq (struct ox_mq_queue *q)
{
    pthread_mutex_destroy (&q->cq_free_mutex);
    pthread_mutex_destroy (&q->cq_used_mutex);
    pthread_mutex_destroy (&q->cq_cond_m);
    pthread_cond_destroy (&q->cq_cond);
}

static int ox_mq_init_sq (struct ox_mq_queue *q, uint32_t size)
{
    TAILQ_INIT (&q->sq_free);
    TAILQ_INIT (&q->sq_used);
    TAILQ_INIT (&q->sq_wait);
    pthread_mutex_init (&q->sq_free_mutex, NULL);
    pthread_mutex_init (&q->sq_used_mutex, NULL);
    pthread_mutex_init (&q->sq_wait_mutex, NULL);
    pthread_mutex_init (&q->sq_cond_m, NULL);
    pthread_cond_init (&q->sq_cond, NULL);

    q->sq_entries = malloc (sizeof (struct ox_mq_entry) * size);
    if (!q->sq_entries)
        goto CLEAN;

    memset (q->sq_entries, 0, sizeof (struct ox_mq_entry) * size);

    return 0;

CLEAN:
    ox_mq_destroy_sq (q);
    return -1;
}

static int ox_mq_init_cq (struct ox_mq_queue *q, uint32_t size)
{
    TAILQ_INIT (&q->cq_free);
    TAILQ_INIT (&q->cq_used);
    pthread_mutex_init (&q->cq_free_mutex, NULL);
    pthread_mutex_init (&q->cq_used_mutex, NULL);
    pthread_mutex_init (&q->cq_cond_m, NULL);
    pthread_cond_init (&q->cq_cond, NULL);

    q->cq_entries = malloc (sizeof (struct ox_mq_entry) * size);
    if (!q->cq_entries)
        goto CLEAN;

    memset (q->cq_entries, 0, sizeof (struct ox_mq_entry) * size);

    return 0;

CLEAN:
    ox_mq_destroy_cq (q);
    return -1;
}

static int ox_mq_init_queue (struct ox_mq_queue *q, uint32_t size,
                                        ox_mq_sq_fn *sq_fn, ox_mq_cq_fn *cq_fn)
{
    int i;

    if (!sq_fn || !cq_fn)
        return -1;

    q->sq_fn = sq_fn;
    q->cq_fn = cq_fn;

    if (ox_mq_init_sq (q, size))
        return -1;

    if (ox_mq_init_cq (q, size))
        goto CLEAN_SQ;

    q->stats.cq_free.counter = U_ATOMIC_INIT_RUNTIME(0);
    q->stats.cq_used.counter = U_ATOMIC_INIT_RUNTIME(0);
    q->stats.sq_free.counter = U_ATOMIC_INIT_RUNTIME(0);
    q->stats.sq_used.counter = U_ATOMIC_INIT_RUNTIME(0);
    q->stats.sq_wait.counter = U_ATOMIC_INIT_RUNTIME(0);

    for (i = 0; i < size; i++) {
        TAILQ_INSERT_TAIL (&q->sq_free, &q->sq_entries[i], entry);
        u_atomic_inc(&q->stats.sq_free);
        TAILQ_INSERT_TAIL (&q->cq_free, &q->cq_entries[i], entry);
        u_atomic_inc(&q->stats.cq_free);
    }

    q->running = 1; /* ready */

    return 0;

CLEAN_SQ:
    ox_mq_destroy_sq (q);
    free (q->sq_entries);
    return -1;
}

static void ox_mq_free_queues (struct ox_mq *mq, uint32_t n_queues)
{
    int i;
    struct ox_mq_queue *q;

    for (i = 0; i < n_queues; i++) {
        q = &mq->queues[i];
        q->running = 0; /* stop threads */
        ox_mq_destroy_sq (q);
        free (q->sq_entries);
        ox_mq_destroy_cq (q);
        free (q->cq_entries);
    }
}

#define OX_MQ_ENQUEUE(head, elm, mutex, stat) do {                  \
        pthread_mutex_lock((mutex));                                \
        TAILQ_INSERT_TAIL((head), (elm), entry);                    \
        pthread_mutex_unlock((mutex));                              \
        u_atomic_inc((stat));                                       \
} while (/*CONSTCOND*/0)

#define OX_MQ_DEQUEUE(head, elm, mutex, stats) do {                 \
        pthread_mutex_lock((mutex));                                \
        req = TAILQ_FIRST((head));                                  \
        TAILQ_REMOVE ((head), (elm), entry);                        \
        pthread_mutex_unlock ((mutex));                             \
        u_atomic_dec((stats));                                      \
} while (/*CONSTCOND*/0)

static void *ox_mq_sq_thread (void *arg)
{
    struct ox_mq_queue *q = (struct ox_mq_queue *) arg;
    struct ox_mq_entry *req;

    while (q->running) {
        pthread_mutex_lock(&q->sq_cond_m);

        if (TAILQ_EMPTY (&q->sq_used))
            pthread_cond_wait(&q->sq_cond, &q->sq_cond_m);

        pthread_mutex_unlock(&q->sq_cond_m);

        OX_MQ_DEQUEUE (&q->sq_used, req, &q->sq_used_mutex, &q->stats.sq_used);
        OX_MQ_ENQUEUE (&q->sq_wait, req, &q->sq_wait_mutex, &q->stats.sq_wait);

        q->sq_fn (req);
    }

    return NULL;
}

static void *ox_mq_cq_thread (void *arg)
{
    struct ox_mq_queue *q = (struct ox_mq_queue *) arg;
    struct ox_mq_entry *req;
    void *opaque;

    while (q->running) {
        pthread_mutex_lock(&q->cq_cond_m);

        if (TAILQ_EMPTY (&q->cq_used))
            pthread_cond_wait(&q->cq_cond, &q->cq_cond_m);

        pthread_mutex_unlock(&q->cq_cond_m);

        OX_MQ_DEQUEUE (&q->cq_used, req, &q->cq_used_mutex, &q->stats.cq_used);
        opaque = req->opaque;
        memset (req, 0, sizeof (struct ox_mq_entry));
        OX_MQ_ENQUEUE (&q->cq_free, req, &q->cq_free_mutex, &q->stats.cq_free);

        q->cq_fn (opaque);
    }

    return NULL;
}

static int ox_mq_start_thread (struct ox_mq_queue *q)
{
    if (pthread_create(&q->sq_tid, NULL, ox_mq_sq_thread, q))
        return -1;

    if (pthread_create(&q->cq_tid, NULL, ox_mq_cq_thread, q))
        return -1;

    return 0;
}

int ox_mq_submit_req (struct ox_mq *mq, uint32_t qid, void *opaque)
{
    struct ox_mq_queue *q;
    struct ox_mq_entry *req;
    uint8_t wake = 0;

    if (qid >= mq->config->n_queues)
        return -1;

    q = &mq->queues[qid];

    /* TODO: retry user defined times if queue is full */
    pthread_mutex_lock (&q->sq_free_mutex);
    if (TAILQ_EMPTY (&q->sq_free)) {
        pthread_mutex_unlock (&q->sq_free_mutex);
        return -1;
    }

    req = TAILQ_FIRST (&q->sq_free);
    TAILQ_REMOVE (&q->sq_free, req, entry);
    pthread_mutex_unlock (&q->sq_free_mutex);
    u_atomic_dec(&q->stats.sq_free);

    req->opaque = opaque;
    req->qid = qid;

    pthread_mutex_lock (&q->sq_used_mutex);
    if (TAILQ_EMPTY (&q->sq_used))
        wake++;

    TAILQ_INSERT_TAIL (&q->sq_used, req, entry);
    u_atomic_inc(&q->stats.sq_used);

    if (wake) {
        pthread_mutex_lock (&q->sq_cond_m);
        pthread_cond_signal(&q->sq_cond);
        pthread_mutex_unlock (&q->sq_cond_m);
    }
    pthread_mutex_unlock (&q->sq_used_mutex);

    return 0;
}

int ox_mq_complete_req (struct ox_mq *mq, struct ox_mq_entry *req_sq)
{
    struct ox_mq_queue *q;
    struct ox_mq_entry *req_cq;
    uint8_t wake = 0;

    if (!req_sq || !req_sq->opaque)
        return -1;

    q = &mq->queues[req_sq->qid];

    /* TODO: retry user defined times if queue is full */
    pthread_mutex_lock (&q->cq_free_mutex);
    if (TAILQ_EMPTY (&q->cq_free)) {
        pthread_mutex_unlock (&q->cq_free_mutex);
        log_info (" [ox-mq: WARNING: CQ Full, request not completed.\n");
        return -1;
    }

    req_cq = TAILQ_FIRST (&q->cq_free);
    TAILQ_REMOVE (&q->cq_free, req_cq, entry);
    pthread_mutex_unlock (&q->cq_free_mutex);
    u_atomic_dec(&q->stats.cq_free);

    req_cq->opaque = req_sq->opaque;
    req_cq->qid = req_sq->qid;

    pthread_mutex_lock (&q->sq_wait_mutex);
    TAILQ_REMOVE (&q->sq_wait, req_sq, entry);
    pthread_mutex_unlock (&q->sq_wait_mutex);
    u_atomic_dec(&q->stats.sq_wait);

    memset (req_sq, 0, sizeof (struct ox_mq_entry));
    OX_MQ_ENQUEUE (&q->sq_free, req_sq, &q->sq_free_mutex, &q->stats.sq_free);

    pthread_mutex_lock (&q->cq_used_mutex);
    if (TAILQ_EMPTY (&q->cq_used))
        wake++;

    TAILQ_INSERT_TAIL (&q->cq_used, req_cq, entry);
    u_atomic_inc(&q->stats.cq_used);

    if (wake) {
        pthread_mutex_lock (&q->cq_cond_m);
        pthread_cond_signal(&q->cq_cond);
        pthread_mutex_unlock (&q->cq_cond_m);
    }
    pthread_mutex_unlock (&q->cq_used_mutex);

    return 0;
}

static void *ox_mq_to_thread (void *arg)
{
    struct ox_mq *mq = (struct ox_mq *) arg;
    int exit, i;
    
    do {
        usleep (mq->config->to_usec);
        // add btree library
        // create btree timeout 
        // create btree for ext_entries
        
        // verify all wait queues
        // enqueue timeout entry to to_btree
        // allocate new entry and enqueue pointer to ext_entries btree
        // also enqueue new entry to sq_free list        
        // complete request if flag is active
        // call user to_fn
        
        // in the completion fn, check if to_btree contains the pointer,
        // if yes, remove from to_btree and free if ext_entries contains the ptr
        //   remove from ext_entries if freed
        
        // create status for timeout entries, timeout completion hits, btrees 
        
        // make volt + core timeout functions
        
        exit = mq->config->n_queues;
        for (i = 0; i < mq->config->n_queues; i++) {
            if (!mq->queues[i].running)
                exit--;
        }
        if (!exit)
            break;
    } while (1);
    
    return NULL;
}

struct ox_mq *ox_mq_init (struct ox_mq_config *config)
{
    int i;

    if (config->q_size < 1 || config->q_size > 0x10000 ||
                            config->n_queues < 1 || config->n_queues > 0x10000)
        return NULL;

    struct ox_mq *mq = malloc (sizeof (struct ox_mq));
    if (!mq)
        return NULL;

    mq->queues = malloc (sizeof (struct ox_mq_queue) * config->n_queues);
    if (!mq->queues)
        goto CLEAN_MQ;
    memset (mq->queues, 0, sizeof (struct ox_mq_queue) * config->n_queues);

    for (i = 0; i < config->n_queues; i++) {
        if (ox_mq_init_queue (&mq->queues[i], config->q_size, 
                                               config->sq_fn, config->cq_fn)) {
            ox_mq_free_queues (mq, i);
            goto CLEAN_Q;
        }

        if (ox_mq_start_thread (&mq->queues[i])) {
            ox_mq_free_queues (mq, i + 1);
            goto CLEAN_Q;
        }
    }
    
    if (pthread_create(&mq->to_tid, NULL, ox_mq_to_thread, mq))
        goto CLEAN_ALL;
    
    mq->config = config;

    log_info (" [ox-mq: Multi queue started (nq: %d, qs: %d)\n", 
                                             config->n_queues, config->q_size);
    return mq;

CLEAN_ALL:
    ox_mq_free_queues (mq, config->n_queues);
CLEAN_Q:
    free (mq->queues);
CLEAN_MQ:
    free (mq);
    return NULL;
}

void ox_mq_destroy (struct ox_mq *mq)
{    
    ox_mq_free_queues(mq, mq->config->n_queues);
    pthread_join (mq->to_tid, NULL);
    free (mq->queues);
    free (mq);
}
