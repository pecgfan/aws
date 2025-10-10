/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#include <apr_lib.h>
#include <apr_atomic.h>
#include <apr_strings.h>
#include <apr_time.h>
#include <apr_buckets.h>
#include <apr_thread_mutex.h>
#include <apr_thread_cond.h>

#include <httpd.h>
#include <http_protocol.h>
#include <http_log.h>

#include "h2_private.h"
#include "h2_conn_ctx.h"
#include "h2_util.h"
#include "h2_bucket_beam.h"


#define H2_BLIST_INIT(b)        APR_RING_INIT(&(b)->list, apr_bucket, link);
#define H2_BLIST_SENTINEL(b)    APR_RING_SENTINEL(&(b)->list, apr_bucket, link)
#define H2_BLIST_EMPTY(b)       APR_RING_EMPTY(&(b)->list, apr_bucket, link)
#define H2_BLIST_FIRST(b)       APR_RING_FIRST(&(b)->list)
#define H2_BLIST_LAST(b)	APR_RING_LAST(&(b)->list)
#define H2_BLIST_INSERT_HEAD(b, e) do {				\
	apr_bucket *ap__b = (e);                                        \
	APR_RING_INSERT_HEAD(&(b)->list, ap__b, apr_bucket, link);	\
    } while (0)
#define H2_BLIST_INSERT_TAIL(b, e) do {				\
	apr_bucket *ap__b = (e);					\
	APR_RING_INSERT_TAIL(&(b)->list, ap__b, apr_bucket, link);	\
    } while (0)
#define H2_BLIST_CONCAT(a, b) do {					\
        APR_RING_CONCAT(&(a)->list, &(b)->list, apr_bucket, link);	\
    } while (0)
#define H2_BLIST_PREPEND(a, b) do {					\
        APR_RING_PREPEND(&(a)->list, &(b)->list, apr_bucket, link);	\
    } while (0)


static void h2_beam_emitted(h2_bucket_beam *beam, h2_beam_proxy *proxy);

#define H2_BPROXY_NEXT(e)             APR_RING_NEXT((e), link)
#define H2_BPROXY_PREV(e)             APR_RING_PREV((e), link)
#define H2_BPROXY_REMOVE(e)           APR_RING_REMOVE((e), link)

#define H2_BPROXY_LIST_INIT(b)        APR_RING_INIT(&(b)->list, h2_beam_proxy, link);
#define H2_BPROXY_LIST_SENTINEL(b)    APR_RING_SENTINEL(&(b)->list, h2_beam_proxy, link)
#define H2_BPROXY_LIST_EMPTY(b)       APR_RING_EMPTY(&(b)->list, h2_beam_proxy, link)
#define H2_BPROXY_LIST_FIRST(b)       APR_RING_FIRST(&(b)->list)
#define H2_BPROXY_LIST_LAST(b)	      APR_RING_LAST(&(b)->list)
#define H2_PROXY_BLIST_INSERT_HEAD(b, e) do {				\
	h2_beam_proxy *ap__b = (e);                                        \
	APR_RING_INSERT_HEAD(&(b)->list, ap__b, h2_beam_proxy, link);	\
    } while (0)
#define H2_BPROXY_LIST_INSERT_TAIL(b, e) do {				\
	h2_beam_proxy *ap__b = (e);					\
	APR_RING_INSERT_TAIL(&(b)->list, ap__b, h2_beam_proxy, link);	\
    } while (0)
#define H2_BPROXY_LIST_CONCAT(a, b) do {					\
        APR_RING_CONCAT(&(a)->list, &(b)->list, h2_beam_proxy, link);	\
    } while (0)
#define H2_BPROXY_LIST_PREPEND(a, b) do {					\
        APR_RING_PREPEND(&(a)->list, &(b)->list, h2_beam_proxy, link);	\
    } while (0)


/*******************************************************************************
 * beam bucket with reference to beam and bucket it represents
 ******************************************************************************/

const apr_bucket_type_t h2_bucket_type_beam;

#define H2_BUCKET_IS_BEAM(e)     (e->type == &h2_bucket_type_beam)

struct h2_beam_proxy {
    apr_bucket_refcount refcount;
    APR_RING_ENTRY(h2_beam_proxy) link;
    h2_bucket_beam *beam;
    apr_bucket *bsender;
    apr_size_t n;
};

static const char Dummy = '\0';

static apr_status_t beam_bucket_read(apr_bucket *b, const char **str, 
                                     apr_size_t *len, apr_read_type_e block)
{
    h2_beam_proxy *d = b->data;
    if (d->bsender) {
        const char *data;
        apr_status_t status = apr_bucket_read(d->bsender, &data, len, block);
        if (status == APR_SUCCESS) {
            *str = data + b->start;
            *len = b->length;
        }
        return status;
    }
    *str = &Dummy;
    *len = 0;
    return APR_ECONNRESET;
}

static void beam_bucket_destroy(void *data)
{
    h2_beam_proxy *d = data;

    if (apr_bucket_shared_destroy(d)) {
        /* When the beam gets destroyed before this bucket, it will
         * NULLify `d->beam` during its destruction. Otherwise, with
         * the beam still alive, we notify it that this receiver bucket
         * is being destroyed and any sender buckets that caused it to
         * exist, may now go. */
        if (d->beam) {
            h2_beam_emitted(d->beam, d);
        }
        apr_bucket_free(d);
    }
}

static apr_bucket * h2_beam_bucket_make(apr_bucket *b, 
                                        h2_bucket_beam *beam,
                                        apr_bucket *bsender, apr_size_t n)
{
    h2_beam_proxy *d;

    d = apr_bucket_alloc(sizeof(*d), b->list);
    H2_BPROXY_LIST_INSERT_TAIL(&beam->proxies, d);
    d->beam = beam;
    d->bsender = bsender;
    d->n = n;
    
    b = apr_bucket_shared_make(b, d, 0, bsender? bsender->length : 0);
    b->type = &h2_bucket_type_beam;

    return b;
}

static apr_bucket *h2_beam_bucket_create(h2_bucket_beam *beam,
                                         apr_bucket *bsender,
                                         apr_bucket_alloc_t *list,
                                         apr_size_t n)
{
    apr_bucket *b = apr_bucket_alloc(sizeof(*b), list);

    APR_BUCKET_INIT(b);
    b->free = apr_bucket_free;
    b->list = list;
    return h2_beam_bucket_make(b, beam, bsender, n);
}

static apr_off_t h2_beam_bucket_mem_used(apr_bucket *b)
{
    h2_beam_proxy *d = b->data;
    if (d->bsender) {
        if (APR_BUCKET_IS_FILE(d->bsender) || APR_BUCKET_IS_MMAP(d->bsender)) {
            return 0;
        }
    }
    return (apr_off_t)b->length;
}

const apr_bucket_type_t h2_bucket_type_beam = {
    "BEAMB", 5, APR_BUCKET_DATA,
    beam_bucket_destroy,
    beam_bucket_read,
    apr_bucket_setaside_noop,
    apr_bucket_shared_split,
    apr_bucket_shared_copy
};

/* registry for bucket converting `h2_bucket_beamer` functions */
static apr_array_header_t *beamers;

static apr_status_t cleanup_beamers(void *dummy)
{
    (void)dummy;
    beamers = NULL;
    return APR_SUCCESS;
}

void h2_register_bucket_beamer(h2_bucket_beamer *beamer)
{
    if (!beamers) {
        apr_pool_cleanup_register(apr_hook_global_pool, NULL,
                                  cleanup_beamers, apr_pool_cleanup_null);
        beamers = apr_array_make(apr_hook_global_pool, 10, 
                                 sizeof(h2_bucket_beamer*));
    }
    APR_ARRAY_PUSH(beamers, h2_bucket_beamer*) = beamer;
}

static apr_bucket *h2_beam_bucket(h2_bucket_beam *beam, 
                                  apr_bucket_brigade *dest,
                                  const apr_bucket *src)
{
    apr_bucket *b = NULL;
    int i;
    if (beamers) {
        for (i = 0; i < beamers->nelts && b == NULL; ++i) {
            h2_bucket_beamer *beamer;
            
            beamer = APR_ARRAY_IDX(beamers, i, h2_bucket_beamer*);
            b = beamer(beam, dest, src);
        }
    }
    return b;
}

static int is_empty(h2_bucket_beam *beam);
static apr_off_t get_buffered_data_len(h2_bucket_beam *beam);

#define H2_BEAM_LOG(beam, c, level, rv, msg) \
    do { \
        if (APLOG_C_IS_LEVEL((c),(level))) { \
            h2_conn_ctx_t *bl_ctx = h2_conn_ctx_get(c); \
            ap_log_cerror(APLOG_MARK, (level), rv, (c), \
                          "BEAM[%s,%s%s%sdata=%ld] conn=%s: %s", \
                          (beam)->name, \
                          (beam)->closed? "closed," : "", \
                          (beam)->aborted? "aborted," : "", \
                          is_empty(beam)? "empty," : "", \
                          (long)get_buffered_data_len(beam), \
                          bl_ctx? bl_ctx->id : "???", \
                          (msg)); \
        } \
    } while (0)



static apr_off_t bucket_mem_used(apr_bucket *b)
{
    if (APR_BUCKET_IS_FILE(b) || APR_BUCKET_IS_MMAP(b)) {
        return 0;
    }
    else if (H2_BUCKET_IS_BEAM(b)) {
        return h2_beam_bucket_mem_used(b);
    }
    else {
        /* should all have determinate length */
        return (apr_off_t)b->length;
    }
}

static int report_consumption(h2_bucket_beam *beam, int locked)
{
    int rv = 0;
    apr_off_t len = beam->received_bytes - beam->cons_bytes_reported;
    h2_beam_io_callback *cb = beam->cons_io_cb;
     
    if (len > 0) {
        if (cb) {
            void *ctx = beam->cons_ctx;
            
            if (locked) apr_thread_mutex_unlock(beam->lock);
            cb(ctx, beam, len);
            if (locked) apr_thread_mutex_lock(beam->lock);
            rv = 1;
        }
        beam->cons_bytes_reported += len;
    }
    return rv;
}

static apr_size_t calc_buffered(h2_bucket_beam *beam)
{
    apr_size_t len = 0;
    apr_bucket *b;
    for (b = H2_BLIST_FIRST(&beam->send_list); 
         b != H2_BLIST_SENTINEL(&beam->send_list);
         b = APR_BUCKET_NEXT(b)) {
        if (b->length == ((apr_size_t)-1)) {
            /* do not count */
        }
        else if (APR_BUCKET_IS_FILE(b) || APR_BUCKET_IS_MMAP(b)) {
            /* if unread, has no real mem footprint. */
        }
        else {
            len += b->length;
        }
    }
    return len;
}

static void r_purge_sent(h2_bucket_beam *beam)
{
    apr_bucket *b;
    /* delete all sender buckets in purge brigade, needs to be called
     * from sender thread only */
    while (!H2_BLIST_EMPTY(&beam->purge_list)) {
        b = H2_BLIST_FIRST(&beam->purge_list);
        apr_bucket_delete(b);
    }
}

static apr_size_t calc_space_left(h2_bucket_beam *beam)
{
    if (beam->max_buf_size > 0) {
        apr_size_t len = calc_buffered(beam);
        return (beam->max_buf_size > len? (beam->max_buf_size - len) : 0);
    }
    return APR_SIZE_MAX;
}

static int buffer_is_empty(h2_bucket_beam *beam)
{
    return ((!beam->recv_buffer || APR_BRIGADE_EMPTY(beam->recv_buffer))
            && H2_BLIST_EMPTY(&beam->send_list));
}

static apr_status_t wait_empty(h2_bucket_beam *beam, apr_read_type_e block)
{
    apr_status_t rv = APR_SUCCESS;
    
    while (!buffer_is_empty(beam) && APR_SUCCESS == rv) {
        if (APR_BLOCK_READ != block) {
            rv = APR_EAGAIN;
        }
        else if (beam->timeout > 0) {
            rv = apr_thread_cond_timedwait(beam->change, beam->lock, beam->timeout);
        }
        else {
            rv = apr_thread_cond_wait(beam->change, beam->lock);
        }
    }
    return rv;
}

static apr_status_t wait_not_empty(h2_bucket_beam *beam, apr_read_type_e block)
{
    apr_status_t rv = APR_SUCCESS;
    
    while (buffer_is_empty(beam) && APR_SUCCESS == rv) {
        if (beam->aborted) {
            rv = APR_ECONNABORTED;
        }
        else if (beam->closed) {
            rv = APR_EOF;
        }
        else if (APR_BLOCK_READ != block) {
            rv = APR_EAGAIN;
        }
        else if (beam->timeout > 0) {
            rv = apr_thread_cond_timedwait(beam->change, beam->lock, beam->timeout);
        }
        else {
            rv = apr_thread_cond_wait(beam->change, beam->lock);
        }
    }
    return rv;
}

static apr_status_t wait_not_full(h2_bucket_beam *beam, apr_read_type_e block, 
                                  apr_size_t *pspace_left)
{
    apr_status_t rv = APR_SUCCESS;
    apr_size_t left;
    
    while (0 == (left = calc_space_left(beam)) && APR_SUCCESS == rv) {
        if (beam->aborted) {
            rv = APR_ECONNABORTED;
        }
        else if (block != APR_BLOCK_READ) {
            rv = APR_EAGAIN;
        }
        else {
            if (beam->send_block_cb) {
                beam->send_block_cb(beam->send_block_ctx, beam);
            }
            if (beam->timeout > 0) {
                rv = apr_thread_cond_timedwait(beam->change, beam->lock, beam->timeout);
            }
            else {
                rv = apr_thread_cond_wait(beam->change, beam->lock);
            }
        }
    }
    *pspace_left = left;
    return rv;
}

static void h2_beam_emitted(h2_bucket_beam *beam, h2_beam_proxy *proxy)
{
    apr_bucket *b, *next;

    /* A proxy bucket on the receiving side has been handled and is being
     * destroyed. Check for all sender buckets in out hold that we no longer
     * need and move them to our purge list. */
    apr_thread_mutex_lock(beam->lock);
    H2_BPROXY_REMOVE(proxy);
    if (proxy->bsender) {
        /* this bucket is a proxy for a sender's bucket, and that
         * should wait in the beam's hold. */
        for (b = H2_BLIST_FIRST(&beam->hold_list);
             b != H2_BLIST_SENTINEL(&beam->hold_list);
             b = APR_BUCKET_NEXT(b)) {
             if (b == proxy->bsender) {
                break;
             }
        }
        if (b != H2_BLIST_SENTINEL(&beam->hold_list)) {
            /* found the sender bucket in hold that is no longer
             * needed. In addition, all hold buckets before this
             * one are also no longer needed.
             * Not all buckets in hold have a proxy, so we move
             * the hold from head up to the found sender onto
             * the beams purge list. */
            for (b = H2_BLIST_FIRST(&beam->hold_list);
                 b != H2_BLIST_SENTINEL(&beam->hold_list);
                 b = next) {
                next = APR_BUCKET_NEXT(b);
                if (b == proxy->bsender) {
                    APR_BUCKET_REMOVE(b);
                    H2_BLIST_INSERT_TAIL(&beam->purge_list, b);
                    break;
                }
                else if (APR_BUCKET_IS_METADATA(b)) {
                    APR_BUCKET_REMOVE(b);
                    H2_BLIST_INSERT_TAIL(&beam->purge_list, b);
                }
                else {
                    /* another data bucket before this one in hold. this
                     * is normal since DATA buckets may be destroyed
                     * out of order (as long as they do not jump over
                     * meta buckets). */
                }
            }
            proxy->bsender = NULL;
        }
        else {
            /* it should be there unless we screwed up */
            ap_log_perror(APLOG_MARK, APLOG_WARNING, 0, beam->pool,
                          APLOGNO(03384) "h2_beam(%d-%s): emitted bucket not "
                          "in hold, n=%d", beam->id, beam->name,
                          (int)proxy->n);
            ap_assert(!proxy->bsender);
        }
    }
    apr_thread_cond_broadcast(beam->change);
    apr_thread_mutex_unlock(beam->lock);
}

static void h2_blist_cleanup(h2_blist *bl)
{
    apr_bucket *e;

    while (!H2_BLIST_EMPTY(bl)) {
        e = H2_BLIST_FIRST(bl);
        apr_bucket_delete(e);
    }
}

int h2_beam_is_closed(h2_bucket_beam *beam)
{
    return beam->closed;
}

static int pool_register(h2_bucket_beam *beam, apr_pool_t *pool, 
                         apr_status_t (*cleanup)(void *))
{
    if (pool && pool != beam->pool) {
        apr_pool_pre_cleanup_register(pool, beam, cleanup);
        return 1;
    }
    return 0;
}

static int pool_kill(h2_bucket_beam *beam, apr_pool_t *pool,
                     apr_status_t (*cleanup)(void *)) {
    if (pool && pool != beam->pool) {
        apr_pool_cleanup_kill(pool, beam, cleanup);
        return 1;
    }
    return 0;
}

static apr_status_t beam_send_cleanup(void *data)
{
    h2_bucket_beam *beam = data;
    /* sender is going away, clear up all references to its memory */
    r_purge_sent(beam);
    h2_blist_cleanup(&beam->send_list);
    report_consumption(beam, 0);
    while (!H2_BPROXY_LIST_EMPTY(&beam->proxies)) {
        h2_beam_proxy *proxy = H2_BPROXY_LIST_FIRST(&beam->proxies);
        H2_BPROXY_REMOVE(proxy);
        proxy->beam = NULL;
        proxy->bsender = NULL;
    }
    h2_blist_cleanup(&beam->purge_list);
    h2_blist_cleanup(&beam->hold_list);
    return APR_SUCCESS;
}

static void recv_buffer_cleanup(h2_bucket_beam *beam)
{
    if (beam->recv_buffer && !APR_BRIGADE_EMPTY(beam->recv_buffer)) {
        apr_bucket_brigade *bb = beam->recv_buffer;
        apr_off_t bblen = 0;
        
        beam->recv_buffer = NULL;
        apr_brigade_length(bb, 0, &bblen);
        beam->received_bytes += bblen;
        
        /* need to do this unlocked since bucket destroy might 
         * call this beam again. */
        apr_thread_mutex_unlock(beam->lock);
        apr_brigade_destroy(bb);
        apr_thread_mutex_lock(beam->lock);

        apr_thread_cond_broadcast(beam->change);
        if (beam->cons_ev_cb) { 
            beam->cons_ev_cb(beam->cons_ctx, beam);
        }
    }
}

static apr_status_t beam_cleanup(h2_bucket_beam *beam, int from_pool)
{
    /*
     * Owner of the beam is going away, depending on which side it owns,
     * cleanup strategies will differ.
     *
     * In general, receiver holds references to memory from sender.
     * Clean up receiver first, if safe, then cleanup sender, if safe.
     */

     /* When called from pool destroy, io callbacks are disabled */
    if (from_pool) {
        beam->cons_io_cb = NULL;
    }
    beam->recv_buffer = NULL;
    beam->recv_pool = NULL;
    pool_kill(beam, beam->pool, beam_send_cleanup);
    return beam_send_cleanup(beam);
}

static apr_status_t beam_pool_cleanup(void *data)
{
    return beam_cleanup(data, 1);
}

apr_status_t h2_beam_destroy(h2_bucket_beam *beam, conn_rec *c)
{
    apr_pool_cleanup_kill(beam->pool, beam, beam_pool_cleanup);
    H2_BEAM_LOG(beam, c, APLOG_TRACE2, 0, "destroy");
    return beam_cleanup(beam, 0);
}

apr_status_t h2_beam_create(h2_bucket_beam **pbeam, conn_rec *from,
                            apr_pool_t *pool, int id, const char *tag,
                            apr_size_t max_buf_size,
                            apr_interval_time_t timeout)
{
    h2_bucket_beam *beam;
    h2_conn_ctx_t *conn_ctx = h2_conn_ctx_get(from);
    apr_status_t rv;
    
    beam = apr_pcalloc(pool, sizeof(*beam));
    beam->pool = pool;
    beam->from = from;
    beam->id = id;
    if (from->master) {
        beam->name = apr_psprintf(pool, "%s-%s", conn_ctx->id, tag);
    }
    else {
        beam->name = apr_psprintf(pool, "%s-%d-%s", conn_ctx->id, id, tag);
    }
    pool_register(beam, beam->pool, beam_send_cleanup);

    H2_BLIST_INIT(&beam->send_list);
    H2_BLIST_INIT(&beam->hold_list);
    H2_BLIST_INIT(&beam->purge_list);
    H2_BPROXY_LIST_INIT(&beam->proxies);
    beam->tx_mem_limits = 1;
    beam->max_buf_size = max_buf_size;
    beam->timeout = timeout;

    rv = apr_thread_mutex_create(&beam->lock, APR_THREAD_MUTEX_DEFAULT, pool);
    if (APR_SUCCESS != rv) goto cleanup;
    rv = apr_thread_cond_create(&beam->change, pool);
    if (APR_SUCCESS != rv) goto cleanup;
    apr_pool_pre_cleanup_register(pool, beam, beam_pool_cleanup);

cleanup:
    H2_BEAM_LOG(beam, from, APLOG_TRACE2, rv, "created");
    *pbeam = (APR_SUCCESS == rv)? beam : NULL;
    return rv;
}

void h2_beam_buffer_size_set(h2_bucket_beam *beam, apr_size_t buffer_size)
{
    apr_thread_mutex_lock(beam->lock);
    beam->max_buf_size = buffer_size;
    apr_thread_mutex_unlock(beam->lock);
}

void h2_beam_set_copy_files(h2_bucket_beam * beam, int enabled)
{
    apr_thread_mutex_lock(beam->lock);
    beam->copy_files = enabled;
    apr_thread_mutex_unlock(beam->lock);
}

apr_size_t h2_beam_buffer_size_get(h2_bucket_beam *beam)
{
    apr_size_t buffer_size = 0;
    
    apr_thread_mutex_lock(beam->lock);
    buffer_size = beam->max_buf_size;
    apr_thread_mutex_unlock(beam->lock);
    return buffer_size;
}

void h2_beam_timeout_set(h2_bucket_beam *beam, apr_interval_time_t timeout)
{
    apr_thread_mutex_lock(beam->lock);
    beam->timeout = timeout;
    apr_thread_mutex_unlock(beam->lock);
}

void h2_beam_abort(h2_bucket_beam *beam, conn_rec *c)
{
    apr_thread_mutex_lock(beam->lock);
    beam->aborted = 1;
    if (c == beam->from) {
        /* sender aborts */
        if (beam->was_empty_cb && buffer_is_empty(beam)) {
            beam->was_empty_cb(beam->was_empty_ctx, beam);
        }
        /* no more consumption reporting to sender */
        beam->cons_ev_cb = NULL;
        beam->cons_io_cb = NULL;
        beam->cons_ctx = NULL;
        r_purge_sent(beam);
        h2_blist_cleanup(&beam->send_list);
        report_consumption(beam, 1);
    }
    else {
        /* receiver aborts */
        recv_buffer_cleanup(beam);
    }
    apr_thread_cond_broadcast(beam->change);
    apr_thread_mutex_unlock(beam->lock);
}

apr_status_t h2_beam_close(h2_bucket_beam *beam, conn_rec *c)
{
    apr_status_t rv;

    apr_thread_mutex_lock(beam->lock);
    H2_BEAM_LOG(beam, c, APLOG_TRACE2, 0, "start close");
    beam->closed = 1;
    if (beam->from == c) {
        /* sender closes, receiver may still read */
        r_purge_sent(beam);
        report_consumption(beam, 1);
        if (beam->was_empty_cb && buffer_is_empty(beam)) {
            beam->was_empty_cb(beam->was_empty_ctx, beam);
        }
        apr_thread_cond_broadcast(beam->change);
    }
    else {
        /* receiver closes, equal to an abort */
        recv_buffer_cleanup(beam);
        beam->aborted = 1;
    }
    rv = beam->aborted? APR_ECONNABORTED : APR_SUCCESS;
    H2_BEAM_LOG(beam, c, APLOG_TRACE2, rv, "end close");
    apr_thread_mutex_unlock(beam->lock);
    return rv;
}

apr_status_t h2_beam_wait_empty(h2_bucket_beam *beam, apr_read_type_e block)
{
    apr_status_t status;

    apr_thread_mutex_lock(beam->lock);
    status = wait_empty(beam, block);
    apr_thread_mutex_unlock(beam->lock);
    return status;
}

static void move_to_hold(h2_bucket_beam *beam, 
                         apr_bucket_brigade *sender_bb)
{
    apr_bucket *b;
    while (sender_bb && !APR_BRIGADE_EMPTY(sender_bb)) {
        b = APR_BRIGADE_FIRST(sender_bb);
        APR_BUCKET_REMOVE(b);
        H2_BLIST_INSERT_TAIL(&beam->send_list, b);
    }
}

static apr_status_t append_bucket(h2_bucket_beam *beam, 
                                  apr_bucket *b,
                                  apr_read_type_e block,
                                  apr_size_t *pspace_left)
{
    const char *data;
    apr_size_t len;
    apr_status_t status = APR_SUCCESS;
    int can_beam = 0, check_len;
    
    (void)block;
    if (beam->aborted) {
        return APR_ECONNABORTED;
    }
    
    if (APR_BUCKET_IS_METADATA(b)) {
        if (APR_BUCKET_IS_EOS(b)) {
            /*beam->closed = 1;*/
        }
        APR_BUCKET_REMOVE(b);
        apr_bucket_setaside(b, beam->pool);
        H2_BLIST_INSERT_TAIL(&beam->send_list, b);
        return APR_SUCCESS;
    }
    else if (APR_BUCKET_IS_FILE(b)) {
        /* For file buckets the problem is their internal readpool that
         * is used on the first read to allocate buffer/mmap.
         * Since setting aside a file bucket will de-register the
         * file cleanup function from the previous pool, we need to
         * call that only from the sender thread.
         *
         * Currently, we do not handle file bucket with refcount > 1 as
         * the beam is then not in complete control of the file's lifetime.
         * Which results in the bug that a file get closed by the receiver
         * while the sender or the beam still have buckets using it. 
         * 
         * Additionally, we allow callbacks to prevent beaming file
         * handles across. The use case for this is to limit the number 
         * of open file handles and rather use a less efficient beam
         * transport. */
        apr_bucket_file *bf = b->data;
        can_beam = !beam->copy_files && (bf->refcount.refcount == 1);
        check_len = !can_beam;
    }
    else if (APR_BUCKET_IS_MMAP(b)) {
        can_beam = !beam->copy_files;
        check_len = !can_beam;
    }
    else {
        if (b->length == ((apr_size_t)-1)) {
            const char *data2;
            status = apr_bucket_read(b, &data2, &len, APR_BLOCK_READ);
            if (status != APR_SUCCESS) {
                return status;
            }
        }
        check_len = 1;
    }
    
    if (check_len) {
        if (b->length > *pspace_left) {
            apr_bucket_split(b, *pspace_left);
        }
        *pspace_left -= b->length;
    }

    /* The fundamental problem is that reading a sender bucket from
     * a receiver thread is a total NO GO, because the bucket might use
     * its pool/bucket_alloc from a foreign thread and that will
     * corrupt. */
    if (b->length == 0) {
        apr_bucket_delete(b);
        return APR_SUCCESS;
    }
    else if (APR_BUCKET_IS_HEAP(b)) {
        /* For heap buckets read from a receiver thread is fine. The
         * data will be there and live until the bucket itself is
         * destroyed. */
        status = apr_bucket_setaside(b, beam->pool);
        if (status != APR_SUCCESS) goto cleanup;
    }
    else if (can_beam && (APR_BUCKET_IS_FILE(b) || APR_BUCKET_IS_MMAP(b))) {
        status = apr_bucket_setaside(b, beam->pool);
        if (status != APR_SUCCESS) goto cleanup;
    }
    else {
        /* we know of no special shortcut to transfer the bucket to
         * another pool without copying. So we make it a heap bucket. */
        status = apr_bucket_read(b, &data, &len, APR_BLOCK_READ);
        if (status != APR_SUCCESS) goto cleanup;
        /* this allocates and copies data */
        apr_bucket_heap_make(b, data, len, NULL);
    }
    
    APR_BUCKET_REMOVE(b);
    H2_BLIST_INSERT_TAIL(&beam->send_list, b);
    beam->sent_bytes += b->length;

cleanup:
    return status;
}

apr_status_t h2_beam_send(h2_bucket_beam *beam, conn_rec *from,
                          apr_bucket_brigade *sender_bb, 
                          apr_read_type_e block)
{
    apr_bucket *b;
    apr_status_t rv = APR_SUCCESS;
    apr_size_t space_left = 0;

    /* Called from the sender thread to add buckets to the beam */
    apr_thread_mutex_lock(beam->lock);
    ap_assert(beam->from == from);
    H2_BEAM_LOG(beam, from, APLOG_TRACE2, rv, "start send");
    r_purge_sent(beam);

    if (beam->aborted) {
        move_to_hold(beam, sender_bb);
        rv = APR_ECONNABORTED;
    }
    else if (beam->closed) {
        /* we just take in buckets after an EOS directly
         * to the hold and do not complain. */
        move_to_hold(beam, sender_bb);
        rv = APR_SUCCESS;
    }
    else if (sender_bb) {
        int was_empty = buffer_is_empty(beam);

        space_left = calc_space_left(beam);
        while (!APR_BRIGADE_EMPTY(sender_bb) && APR_SUCCESS == rv) {
            if (space_left <= 0) {
                r_purge_sent(beam);
                if (was_empty && beam->was_empty_cb) {
                    beam->was_empty_cb(beam->was_empty_ctx, beam);
                }
                rv = wait_not_full(beam, block, &space_left);
                if (APR_SUCCESS != rv) {
                    break;
                }
                was_empty = buffer_is_empty(beam);
            }
            b = APR_BRIGADE_FIRST(sender_bb);
            rv = append_bucket(beam, b, block, &space_left);
        }

        if (was_empty && beam->was_empty_cb && !buffer_is_empty(beam)) {
            beam->was_empty_cb(beam->was_empty_ctx, beam);
        }
        apr_thread_cond_broadcast(beam->change);
    }

    report_consumption(beam, 1);
    H2_BEAM_LOG(beam, from, APLOG_TRACE2, rv, "end send");
    apr_thread_mutex_unlock(beam->lock);
    return rv;
}

apr_status_t h2_beam_receive(h2_bucket_beam *beam,
                             conn_rec *to,
                             apr_bucket_brigade *bb, 
                             apr_read_type_e block,
                             apr_off_t readbytes,
                             int *pclosed)
{
    apr_bucket *bsender, *brecv, *ng;
    int transferred = 0;
    apr_status_t rv = APR_SUCCESS;
    apr_off_t remain;
    int transferred_buckets = 0;

    apr_thread_mutex_lock(beam->lock);
    H2_BEAM_LOG(beam, to, APLOG_TRACE2, 0, "start receive");
    if (readbytes <= 0) {
        readbytes = (apr_off_t)APR_SIZE_MAX;
    }
    remain = readbytes;

transfer:
    if (beam->aborted) {
        recv_buffer_cleanup(beam);
        rv = APR_ECONNABORTED;
        goto leave;
    }

    /* transfer enough buckets from our receiver brigade, if we have one */
    while (remain >= 0
           && beam->recv_buffer
           && !APR_BRIGADE_EMPTY(beam->recv_buffer)) {

        brecv = APR_BRIGADE_FIRST(beam->recv_buffer);
        if (brecv->length > 0 && remain <= 0) {
            break;
        }
        APR_BUCKET_REMOVE(brecv);
        APR_BRIGADE_INSERT_TAIL(bb, brecv);
        remain -= brecv->length;
        ++transferred;
    }

    /* transfer from our sender brigade, transforming sender buckets to
     * receiver ones until we have enough */
    while (remain >= 0 && !H2_BLIST_EMPTY(&beam->send_list)) {

        brecv = NULL;
        bsender = H2_BLIST_FIRST(&beam->send_list);
        if (bsender->length > 0 && remain <= 0) {
            break;
        }

        if (APR_BUCKET_IS_METADATA(bsender)) {
            /* we need a real copy into the receivers bucket_alloc */
            if (APR_BUCKET_IS_EOS(bsender)) {
                brecv = apr_bucket_eos_create(bb->bucket_alloc);
                beam->close_sent = 1;
            }
            else if (APR_BUCKET_IS_FLUSH(bsender)) {
                brecv = apr_bucket_flush_create(bb->bucket_alloc);
            }
            else if (AP_BUCKET_IS_ERROR(bsender)) {
                ap_bucket_error *eb = (ap_bucket_error *)bsender;
                brecv = ap_bucket_error_create(eb->status, eb->data,
                                                bb->p, bb->bucket_alloc);
            }
        }
        else if (bsender->length == 0) {
            APR_BUCKET_REMOVE(bsender);
            H2_BLIST_INSERT_TAIL(&beam->hold_list, bsender);
            continue;
        }
        else if (APR_BUCKET_IS_FILE(bsender)) {
            /* This is setaside into the target brigade pool so that
             * any read operation messes with that pool and not
             * the sender one. */
            apr_bucket_file *f = (apr_bucket_file *)bsender->data;
            apr_file_t *fd = f->fd;
            int setaside = (f->readpool != bb->p);

            if (setaside) {
                rv = apr_file_setaside(&fd, fd, bb->p);
                if (rv != APR_SUCCESS) {
                    goto leave;
                }
            }
            ng = apr_brigade_insert_file(bb, fd, bsender->start, (apr_off_t)bsender->length,
                                         bb->p);
#if APR_HAS_MMAP
            /* disable mmap handling as this leads to segfaults when
             * the underlying file is changed while memory pointer has
             * been handed out. See also PR 59348 */
            apr_bucket_file_enable_mmap(ng, 0);
#endif
            APR_BUCKET_REMOVE(bsender);
            H2_BLIST_INSERT_TAIL(&beam->hold_list, bsender);

            remain -= bsender->length;
            beam->received_bytes += bsender->length;
            ++transferred;
            ++transferred_buckets;
            continue;
        }
        else {
            /* create a "receiver" proxy bucket. we took care about the
             * underlying sender bucket and its data when we placed it into
             * the sender brigade.
             * the beam bucket will notify us on destruction that bsender is
             * no longer needed. */
            brecv = h2_beam_bucket_create(beam, bsender, bb->bucket_alloc,
                                           beam->buckets_sent++);
        }

        /* Place the sender bucket into our hold, to be
         * cleaned up once the receiver destroys its brecv proxy. */
        APR_BUCKET_REMOVE(bsender);
        H2_BLIST_INSERT_TAIL(&beam->hold_list, bsender);
        beam->received_bytes += bsender->length;
        ++transferred_buckets;

        if (brecv) {
            /* we have a proxy that we can give the receiver */
            APR_BRIGADE_INSERT_TAIL(bb, brecv);
            remain -= brecv->length;
            ++transferred;
        }
        else {
            /* Does someone else know how to make a proxy for
             * the bucket? Ask the callbacks registered for this. */
            brecv = h2_beam_bucket(beam, bb, bsender);

            while (brecv && brecv != APR_BRIGADE_SENTINEL(bb)) {
                ++transferred;
                remain -= brecv->length;
                brecv = APR_BUCKET_NEXT(brecv);
            }
        }
    }

    if (remain < 0) {
        /* too much, put some back into out recv_buffer */
        remain = readbytes;
        for (brecv = APR_BRIGADE_FIRST(bb);
             brecv != APR_BRIGADE_SENTINEL(bb);
             brecv = APR_BUCKET_NEXT(brecv)) {
            remain -= (beam->tx_mem_limits? bucket_mem_used(brecv)
                       : (apr_off_t)brecv->length);
            if (remain < 0) {
                apr_bucket_split(brecv, (apr_size_t)((apr_off_t)brecv->length+remain));
                beam->recv_buffer = apr_brigade_split_ex(bb,
                                                         APR_BUCKET_NEXT(brecv),
                                                         beam->recv_buffer);
                break;
            }
        }
    }

    if (beam->closed && buffer_is_empty(beam) && !beam->close_sent) {
        /* beam is closed and we have nothing more to receive */
        apr_bucket *b = apr_bucket_eos_create(bb->bucket_alloc);
        APR_BRIGADE_INSERT_TAIL(bb, b);
        beam->close_sent = 1;
        ++transferred;
        rv = APR_SUCCESS;
    }

    if (beam->cons_ev_cb && transferred_buckets > 0) {
        beam->cons_ev_cb(beam->cons_ctx, beam);
    }

    if (transferred) {
        apr_thread_cond_broadcast(beam->change);
        rv = APR_SUCCESS;
    }
    else if (beam->closed) {
        rv = APR_EOF;
    }
    else {
        rv = wait_not_empty(beam, block);
        if (rv != APR_SUCCESS) {
            goto leave;
        }
        goto transfer;
    }

leave:
    if (pclosed) *pclosed = beam->closed? 1 : 0;
    H2_BEAM_LOG(beam, to, APLOG_TRACE2, rv, "end receive");
    apr_thread_mutex_unlock(beam->lock);
    return rv;
}

void h2_beam_on_consumed(h2_bucket_beam *beam, 
                         h2_beam_ev_callback *ev_cb,
                         h2_beam_io_callback *io_cb, void *ctx)
{
    apr_thread_mutex_lock(beam->lock);
    beam->cons_ev_cb = ev_cb;
    beam->cons_io_cb = io_cb;
    beam->cons_ctx = ctx;
    apr_thread_mutex_unlock(beam->lock);
}

void h2_beam_on_was_empty(h2_bucket_beam *beam,
                          h2_beam_ev_callback *was_empty_cb, void *ctx)
{
    apr_thread_mutex_lock(beam->lock);
    beam->was_empty_cb = was_empty_cb;
    beam->was_empty_ctx = ctx;
    apr_thread_mutex_unlock(beam->lock);
}


void h2_beam_on_send_block(h2_bucket_beam *beam,
                           h2_beam_ev_callback *send_block_cb, void *ctx)
{
    apr_thread_mutex_lock(beam->lock);
    beam->send_block_cb = send_block_cb;
    beam->send_block_ctx = ctx;
    apr_thread_mutex_unlock(beam->lock);
}

static apr_off_t get_buffered_data_len(h2_bucket_beam *beam)
{
    apr_bucket *b;
    apr_off_t l = 0;

    for (b = H2_BLIST_FIRST(&beam->send_list);
        b != H2_BLIST_SENTINEL(&beam->send_list);
        b = APR_BUCKET_NEXT(b)) {
        /* should all have determinate length */
        l += b->length;
    }
    return l;
}

apr_off_t h2_beam_get_buffered(h2_bucket_beam *beam)
{
    apr_off_t l = 0;

    apr_thread_mutex_lock(beam->lock);
    l = get_buffered_data_len(beam);
    apr_thread_mutex_unlock(beam->lock);
    return l;
}

apr_off_t h2_beam_get_mem_used(h2_bucket_beam *beam)
{
    apr_bucket *b;
    apr_off_t l = 0;

    apr_thread_mutex_lock(beam->lock);
    for (b = H2_BLIST_FIRST(&beam->send_list);
        b != H2_BLIST_SENTINEL(&beam->send_list);
        b = APR_BUCKET_NEXT(b)) {
        l += bucket_mem_used(b);
    }
    apr_thread_mutex_unlock(beam->lock);
    return l;
}

static int is_empty(h2_bucket_beam *beam)
{
    return (H2_BLIST_EMPTY(&beam->send_list)
            && (!beam->recv_buffer || APR_BRIGADE_EMPTY(beam->recv_buffer)));
}

int h2_beam_empty(h2_bucket_beam *beam)
{
    int empty = 1;

    apr_thread_mutex_lock(beam->lock);
    empty = is_empty(beam);
    apr_thread_mutex_unlock(beam->lock);
    return empty;
}

int h2_beam_no_files(void *ctx, h2_bucket_beam *beam, apr_file_t *file)
{
    (void)ctx; (void)beam; (void)file;
    return 0;
}

int h2_beam_report_consumption(h2_bucket_beam *beam)
{
    int rv = 0;

    apr_thread_mutex_lock(beam->lock);
    rv = report_consumption(beam, 1);
    apr_thread_mutex_unlock(beam->lock);
    return rv;
}
