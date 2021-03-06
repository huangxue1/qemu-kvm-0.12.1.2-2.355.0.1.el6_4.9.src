/*
 * Live block commit
 *
 * Copyright Red Hat, Inc. 2012
 *
 * Authors:
 *  Jeff Cody   <jcody@redhat.com>
 *  Based on stream.c by Stefan Hajnoczi
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include "trace.h"
#include "block_int.h"
#include "qerror.h"

enum {
    /*
     * Size of data buffer for populating the image file.  This should be large
     * enough to process multiple clusters in a single call, so that populating
     * contiguous regions of the image is efficient.
     */
    COMMIT_BUFFER_SIZE = 512 * 1024, /* in bytes */
};

#define SLICE_TIME 100ULL /* ms */

typedef struct {
    int64_t next_slice_time;
    uint64_t slice_quota;
    uint64_t dispatched;
} RateLimit;

static int64_t ratelimit_calculate_delay(RateLimit *limit, uint64_t n)
{
    int64_t now = qemu_get_clock(rt_clock);

    if (limit->next_slice_time < now) {
        limit->next_slice_time = now + SLICE_TIME;
        limit->dispatched = 0;
    }
    if (limit->dispatched == 0 || limit->dispatched + n <= limit->slice_quota) {
        limit->dispatched += n;
        return 0;
    } else {
        limit->dispatched = n;
        return limit->next_slice_time - now;
    }
}

static void ratelimit_set_speed(RateLimit *limit, uint64_t speed)
{
    limit->slice_quota = speed / (1000ULL / SLICE_TIME);
}

typedef struct CommitBlockJob {
    BlockJob common;
    RateLimit limit;
    BlockDriverState *active;
    BlockDriverState *top;
    BlockDriverState *base;
    BlockErrorAction on_error;
    int base_flags;
    int orig_overlay_flags;
} CommitBlockJob;

static int coroutine_fn commit_populate(BlockDriverState *bs,
                                        BlockDriverState *base,
                                        int64_t sector_num, int nb_sectors,
                                        void *buf)
{
    int ret = 0;

    ret = bdrv_read(bs, sector_num, buf, nb_sectors);
    if (ret) {
        return ret;
    }

    ret = bdrv_write(base, sector_num, buf, nb_sectors);
    if (ret) {
        return ret;
    }

    return 0;
}

static void coroutine_fn commit_run(void *opaque)
{
    CommitBlockJob *s = opaque;
    BlockDriverState *active = s->active;
    BlockDriverState *top = s->top;
    BlockDriverState *base = s->base;
    BlockDriverState *overlay_bs = NULL;
    int64_t sector_num, end;
    int ret = 0;
    int n = 0;
    void *buf;
    int bytes_written = 0;
    int64_t base_len;

    ret = s->common.len = bdrv_getlength(top);


    if (s->common.len < 0) {
        goto exit_restore_reopen;
    }

    ret = base_len = bdrv_getlength(base);
    if (base_len < 0) {
        goto exit_restore_reopen;
    }

    if (base_len < s->common.len) {
        ret = bdrv_truncate(base, s->common.len);
        if (ret) {
            goto exit_restore_reopen;
        }
    }

    overlay_bs = bdrv_find_overlay(active, top);

    end = s->common.len >> BDRV_SECTOR_BITS;
    buf = qemu_blockalign(top, COMMIT_BUFFER_SIZE);

    for (sector_num = 0; sector_num < end; sector_num += n) {
        uint64_t delay_ms = 0;
        bool copy;

wait:
        /* Note that even when no rate limit is applied we need to yield
         * with no pending I/O here so that qemu_aio_flush() returns.
         */
        block_job_sleep(&s->common, rt_clock, delay_ms);
        if (block_job_is_cancelled(&s->common)) {
            break;
        }
        /* Copy if allocated above the base */
        ret = bdrv_co_is_allocated_above(top, base, sector_num,
                                         COMMIT_BUFFER_SIZE / BDRV_SECTOR_SIZE,
                                         &n);
        copy = (ret == 1);
        trace_commit_one_iteration(s, sector_num, n, ret);
        if (copy) {
            if (s->common.speed) {
                delay_ms = ratelimit_calculate_delay(&s->limit, n);
                if (delay_ms > 0) {
                    goto wait;
                }
            }
            ret = commit_populate(top, base, sector_num, n, buf);
            bytes_written += n * BDRV_SECTOR_SIZE;
        }
        if (ret < 0) {
            if (s->on_error == BLOCK_ERR_STOP_ANY    ||
                s->on_error == BLOCK_ERR_REPORT      ||
                (s->on_error == BLOCK_ERR_STOP_ENOSPC && ret == -ENOSPC)) {
                goto exit_free_buf;
            } else {
                n = 0;
                continue;
            }
        }
        /* Publish progress */
        s->common.offset += n * BDRV_SECTOR_SIZE;
    }

    ret = 0;

    if (!block_job_is_cancelled(&s->common) && sector_num == end) {
        /* success */
        ret = bdrv_drop_intermediate(active, top, base);
    }

exit_free_buf:
    qemu_vfree(buf);

exit_restore_reopen:
    /* restore base open flags here if appropriate (e.g., change the base back
     * to r/o). These reopens do not need to be atomic, since we won't abort
     * even on failure here */
    if (s->base_flags != bdrv_get_flags(base)) {
        bdrv_reopen(base, s->base_flags, NULL);
    }
    if (s->orig_overlay_flags != bdrv_get_flags(overlay_bs)) {
        bdrv_reopen(overlay_bs, s->orig_overlay_flags, NULL);
    }

    block_job_complete(&s->common, ret);
}

static int commit_set_speed(BlockJob *job, int64_t speed)
{
    CommitBlockJob *s = container_of(job, CommitBlockJob, common);

    if (speed < 0) {
        return -EINVAL;
    }
    ratelimit_set_speed(&s->limit, speed / BDRV_SECTOR_SIZE);
    return 0;
}

static BlockJobType commit_job_type = {
    .instance_size = sizeof(CommitBlockJob),
    .job_type      = "commit",
    .set_speed     = commit_set_speed,
};

void commit_start(BlockDriverState *bs, BlockDriverState *base,
                  BlockDriverState *top, int64_t speed,
                  BlockErrorAction on_error, BlockDriverCompletionFunc *cb,
                  void *opaque, Error **errp)
{
    CommitBlockJob *s;
    BlockReopenQueue *reopen_queue = NULL;
    int orig_overlay_flags;
    int orig_base_flags;
    BlockDriverState *overlay_bs;
    Error *local_err = NULL;

    if ((on_error == BLOCK_ERR_STOP_ANY ||
         on_error == BLOCK_ERR_STOP_ENOSPC) &&
        !bdrv_iostatus_is_enabled(bs)) {
        error_set(errp, QERR_INVALID_PARAMETER_COMBINATION);
        return;
    }

    /* Once we support top == active layer, remove this check */
    if (top == bs) {
        error_set(errp, QERR_TOP_IS_ACTIVE);
        return;
    }

    if (top == base) {
        error_set(errp, QERR_TOP_AND_BASE_IDENTICAL);
        return;
    }

    overlay_bs = bdrv_find_overlay(bs, top);

    if (overlay_bs == NULL) {
        error_set(errp, QERR_TOP_NOT_FOUND, top->filename);
        return;
    }

    orig_base_flags    = bdrv_get_flags(base);
    orig_overlay_flags = bdrv_get_flags(overlay_bs);

    /* convert base & overlay_bs to r/w, if necessary */
    if (!(orig_base_flags & BDRV_O_RDWR)) {
        reopen_queue = bdrv_reopen_queue(reopen_queue, base,
                                         orig_base_flags | BDRV_O_RDWR);
    }
    if (!(orig_overlay_flags & BDRV_O_RDWR)) {
        reopen_queue = bdrv_reopen_queue(reopen_queue, overlay_bs,
                                         orig_overlay_flags | BDRV_O_RDWR);
    }
    if (reopen_queue) {
        bdrv_reopen_multiple(reopen_queue, &local_err);
        if (local_err != NULL) {
            error_propagate(errp, local_err);
            return;
        }
    }


    s = block_job_create(&commit_job_type, bs, speed, cb, opaque);
    if (!s) {
        error_set(errp, QERR_DEVICE_IN_USE, bs->device_name);
        return;
    }

    s->base   = base;
    s->top    = top;
    s->active = bs;

    s->base_flags          = orig_base_flags;
    s->orig_overlay_flags  = orig_overlay_flags;

    s->on_error = on_error;
    s->common.co = qemu_coroutine_create(commit_run);

    trace_commit_start(bs, base, top, s, s->common.co, opaque);
    qemu_coroutine_enter(s->common.co, s);
}
