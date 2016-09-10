// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2008-2015 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <stdlib.h>
#include <debug.h>
#include <trace.h>
#include <pow2.h>
#include <string.h>
#include <assert.h>
#include <lib/cbuf.h>
#include <kernel/event.h>
#include <kernel/spinlock.h>

#define LOCAL_TRACE 0

#define INC_POINTER(cbuf, ptr, inc) \
    modpow2(((ptr) + (inc)), (cbuf)->len_pow2)

void cbuf_initialize(cbuf_t *cbuf, size_t len)
{
    cbuf_initialize_etc(cbuf, len, malloc(len));
}

void cbuf_initialize_etc(cbuf_t *cbuf, size_t len, void *buf)
{
    DEBUG_ASSERT(cbuf);
    DEBUG_ASSERT(len > 0);
    DEBUG_ASSERT(ispow2(len));

    cbuf->head = 0;
    cbuf->tail = 0;
    cbuf->len_pow2 = log2_uint(len);
    cbuf->buf = buf;
    event_init(&cbuf->event, false, 0);
    spin_lock_init(&cbuf->lock);

    LTRACEF("len %zd, len_pow2 %u\n", len, cbuf->len_pow2);
}

size_t cbuf_space_avail(cbuf_t *cbuf)
{
    uint consumed = modpow2((uint)(cbuf->head - cbuf->tail), cbuf->len_pow2);
    return valpow2(cbuf->len_pow2) - consumed - 1;
}

size_t cbuf_space_used(cbuf_t *cbuf)
{
    return modpow2((uint)(cbuf->head - cbuf->tail), cbuf->len_pow2);
}

size_t cbuf_write_etc(cbuf_t *cbuf, const void *_buf, size_t len, uint32_t flags)
{
    const char *buf = (const char *)_buf;

    LTRACEF("len %zd\n", len);

    DEBUG_ASSERT(cbuf);
    DEBUG_ASSERT(len < valpow2(cbuf->len_pow2));

    spin_lock_saved_state_t state;
    spin_lock_irqsave(&cbuf->lock, state);

    size_t write_len;
    size_t pos = 0;

    while (pos < len && cbuf_space_avail(cbuf) > 0) {
        if (cbuf->head >= cbuf->tail) {
            write_len = MIN(valpow2(cbuf->len_pow2) - cbuf->head, len - pos);
        } else {
            write_len = MIN(cbuf->tail - cbuf->head - 1, len - pos);
        }

        // if it's full, abort and return how much we've written
        if (write_len == 0) {
            break;
        }

        if (NULL == buf) {
            if (!(flags & CBUF_WRITE_FLAG_NOFILL)) {
                memset(cbuf->buf + cbuf->head, 0, write_len);
            }
        } else {
            memcpy(cbuf->buf + cbuf->head, buf + pos, write_len);
        }

        cbuf->head = INC_POINTER(cbuf, cbuf->head, write_len);
        pos += write_len;
    }

    int signaled = 0;
    if (cbuf->head != cbuf->tail)
        signaled = event_signal(&cbuf->event, false);

    spin_unlock_irqrestore(&cbuf->lock, state);

    if ((flags & CBUF_WRITE_FLAG_CANRESCHEDULE) && (signaled > 0))
        thread_preempt(false);

    return pos;
}

size_t cbuf_read(cbuf_t *cbuf, void *_buf, size_t buflen, bool block)
{
    char *buf = (char *)_buf;

    DEBUG_ASSERT(cbuf);

retry:
    // block on the cbuf outside of the lock, which may
    // unblock us early and we'll have to double check below
    if (block)
        event_wait(&cbuf->event);

    spin_lock_saved_state_t state;
    spin_lock_irqsave(&cbuf->lock, state);

    // see if there's data available
    size_t ret = 0;
    if (cbuf->tail != cbuf->head) {
        size_t pos = 0;

        // loop until we've read everything we need
        // at most this will make two passes to deal with wraparound
        while (pos < buflen && cbuf->tail != cbuf->head) {
            size_t read_len;
            if (cbuf->head > cbuf->tail) {
                // simple case where there is no wraparound
                read_len = MIN(cbuf->head - cbuf->tail, buflen - pos);
            } else {
                // read to the end of buffer in this pass
                read_len = MIN(valpow2(cbuf->len_pow2) - cbuf->tail, buflen - pos);
            }

            // Only perform the copy if a buf was supplied
            if (NULL != buf) {
                memcpy(buf + pos, cbuf->buf + cbuf->tail, read_len);
            }

            cbuf->tail = INC_POINTER(cbuf, cbuf->tail, read_len);
            pos += read_len;
        }

        if (cbuf->tail == cbuf->head) {
            DEBUG_ASSERT(pos > 0);
            // we've emptied the buffer, unsignal the event
            event_unsignal(&cbuf->event);
        }

        ret = pos;
    }

    spin_unlock_irqrestore(&cbuf->lock, state);

    // we apparently blocked but raced with another thread and found no data, retry
    if (block && ret == 0)
        goto retry;

    return ret;
}

size_t cbuf_peek_etc(cbuf_t *cbuf, iovec_t *regions, bool writable)
{
    DEBUG_ASSERT(cbuf && regions);

    spin_lock_saved_state_t state;
    spin_lock_irqsave(&cbuf->lock, state);

    size_t sz = cbuf_size(cbuf);
    size_t ret, pos;

    if (writable) {
        DEBUG_ASSERT(cbuf->head < sz);
        ret = cbuf_space_avail(cbuf);
        pos = cbuf->head;
    } else {
        DEBUG_ASSERT(cbuf->tail < sz);
        ret = cbuf_space_used(cbuf);
        pos = cbuf->tail;
    }
    DEBUG_ASSERT(ret <= sz);

    regions[0].iov_base = ret ? (cbuf->buf + pos) : NULL;
    if (ret + pos > sz) {
        regions[0].iov_len  = sz - pos;
        regions[1].iov_base = cbuf->buf;
        regions[1].iov_len  = ret - regions[0].iov_len;
    } else {
        regions[0].iov_len  = ret;
        regions[1].iov_base = NULL;
        regions[1].iov_len  = 0;
    }

    spin_unlock_irqrestore(&cbuf->lock, state);
    return ret;
}

size_t cbuf_write_char(cbuf_t *cbuf, char c, bool canreschedule)
{
    DEBUG_ASSERT(cbuf);

    spin_lock_saved_state_t state;
    spin_lock_irqsave(&cbuf->lock, state);

    size_t ret = 0;
    int signaled = 0;
    if (cbuf_space_avail(cbuf) > 0) {
        cbuf->buf[cbuf->head] = c;

        cbuf->head = INC_POINTER(cbuf, cbuf->head, 1);
        ret = 1;

        if (cbuf->head != cbuf->tail)
            signaled = event_signal(&cbuf->event, false);
    }

    spin_unlock_irqrestore(&cbuf->lock, state);

    if (canreschedule && (signaled > 0))
        thread_preempt(false);

    return ret;
}

size_t cbuf_read_char(cbuf_t *cbuf, char *c, bool block)
{
    DEBUG_ASSERT(cbuf);
    DEBUG_ASSERT(c);

retry:
    if (block)
        event_wait(&cbuf->event);

    spin_lock_saved_state_t state;
    spin_lock_irqsave(&cbuf->lock, state);

    // see if there's data available
    size_t ret = 0;
    if (cbuf->tail != cbuf->head) {

        *c = cbuf->buf[cbuf->tail];
        cbuf->tail = INC_POINTER(cbuf, cbuf->tail, 1);

        if (cbuf->tail == cbuf->head) {
            // we've emptied the buffer, unsignal the event
            event_unsignal(&cbuf->event);
        }

        ret = 1;
    }

    spin_unlock_irqrestore(&cbuf->lock, state);

    if (block && ret == 0)
        goto retry;

    return ret;
}

