#ifndef OX_MQ_H
#define OX_MQ_H

#include <sys/queue.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include "uatomic.h"

enum {
    OX_MQ_FREE = 1,
    OX_MQ_QUEUED,
    OX_MQ_WAITING,
    OX_MQ_TIMEOUT,
    OX_MQ_TIMEOUT_COMPLETED,
    OX_MQ_TIMEOUT_BACK
};

struct ox_mq_entry {
    void                     *opaque;
    uint32_t                 qid;
    uint8_t                  status;
    struct timeval           wtime; /* timestamp for timeout */
    uint8_t                  is_ext; /* if > 0, allocated due timeout */
    TAILQ_ENTRY(ox_mq_entry) entry;
    LIST_ENTRY(ox_mq_entry)  ext_entry;
    pthread_mutex_t          entry_mutex;
};

/* Keeps a set of counters related to the multi-queue */
struct ox_mq_stats {
    u_atomic_t    sq_free;
    u_atomic_t    sq_used;
    u_atomic_t    sq_wait;
    u_atomic_t    cq_free;
    u_atomic_t    cq_used;
    u_atomic_t    ext_list; /* extended entry list size */
    u_atomic_t    timeout;  /* total timeout entries */
    u_atomic_t    to_back;  /* timeout entries asked for a late completion */
};

typedef void (ox_mq_sq_fn)(struct ox_mq_entry *);

/* void * is the pointer to the opaque user entry */
typedef void (ox_mq_cq_fn)(void *);

/* void ** is an array of timeout opaque entries, int is the array size */
typedef void (ox_mq_to_fn)(void **, int);

struct ox_mq_queue {
    pthread_mutex_t                        sq_free_mutex;
    pthread_mutex_t                        cq_free_mutex;
    pthread_mutex_t                        sq_used_mutex;
    pthread_mutex_t                        cq_used_mutex;
    pthread_mutex_t                        sq_wait_mutex;
    struct ox_mq_entry                     *sq_entries;
    struct ox_mq_entry                     *cq_entries;
    TAILQ_HEAD (sq_free_head, ox_mq_entry) sq_free;
    TAILQ_HEAD (sq_used_head, ox_mq_entry) sq_used;
    TAILQ_HEAD (sq_wait_head, ox_mq_entry) sq_wait;
    TAILQ_HEAD (cq_free_head, ox_mq_entry) cq_free;
    TAILQ_HEAD (cq_used_head, ox_mq_entry) cq_used;
    ox_mq_sq_fn                            *sq_fn;
    ox_mq_cq_fn                            *cq_fn;
    pthread_mutex_t                        sq_cond_m;
    pthread_mutex_t                        cq_cond_m;
    pthread_cond_t                         sq_cond;
    pthread_cond_t                         cq_cond;
    pthread_t                              sq_tid;
    pthread_t                              cq_tid;
    uint8_t                                running; /* if 0, kill threads */
    struct ox_mq_stats                     stats;
};

#define OX_MQ_TO_COMPLETE   (1 << 0) /* Complete request after timeout */

struct ox_mq_config {
    uint32_t            n_queues;
    uint32_t            q_size;
    ox_mq_sq_fn         *sq_fn;  /* submission queue consumer */
    ox_mq_cq_fn         *cq_fn;  /* completion queue consumer */
    ox_mq_to_fn         *to_fn;  /* timeout call */
    uint64_t            to_usec; /* timeout in microseconds */
    uint8_t             flags;
};

struct ox_mq {
    struct ox_mq_queue                *queues;
    struct ox_mq_config               *config;
    pthread_t                         to_tid;       /* timeout thread */
    LIST_HEAD(oxmq_ext, ox_mq_entry)  ext_list;     /* new allocated entries */
    struct ox_mq_stats                stats;
};

struct ox_mq *ox_mq_init (struct ox_mq_config *);
void          ox_mq_destroy (struct ox_mq *);
int           ox_mq_submit_req (struct ox_mq *, uint32_t, void *);
int           ox_mq_complete_req (struct ox_mq *, struct ox_mq_entry *);
void          ox_mq_show_stats (struct ox_mq *);

#endif /* OX_MQ_H */
