#ifndef OX_MQ_H
#define OX_MQ_H

#include <sys/queue.h>
#include <pthread.h>
#include <stdint.h>
#include "uatomic.h"

struct ox_mq_entry {
    void                     *opaque;
    uint32_t                 qid;
    uint8_t                  status;
    TAILQ_ENTRY(ox_mq_entry)  entry;
};

struct ox_mq_stats {
    u_atomic_t    sq_free;
    u_atomic_t    sq_used;
    u_atomic_t    sq_wait;
    u_atomic_t    cq_free;
    u_atomic_t    cq_used;
};

typedef void (ox_mq_sq_fn)(struct ox_mq_entry *);
typedef void (ox_mq_cq_fn)(void *);

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
    pthread_t                              sq_tid;
    pthread_t                              cq_tid;
    uint8_t                                running; /* if 0, kill threads */
    struct ox_mq_stats                     stats;
};

struct ox_mq {
    uint32_t            n_queues;
    uint32_t            q_size;
    struct ox_mq_queue  *queues;
};

struct ox_mq *ox_mq_init (uint32_t, uint32_t, ox_mq_sq_fn *, ox_mq_cq_fn *);
void          ox_mq_destroy (struct ox_mq *);
int           ox_mq_submit_req (struct ox_mq *, uint32_t, void *);
int           ox_mq_complete_req (struct ox_mq *, struct ox_mq_entry *);
void          ox_mq_show_stats (struct ox_mq *);

#endif /* OX_MQ_H */
