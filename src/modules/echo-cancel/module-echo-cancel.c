/***
    This file is part of PulseAudio.

    Copyright 2010 Wim Taymans <wim.taymans@gmail.com>

    Based on module-virtual-sink.c
             module-virtual-source.c
             module-loopback.c

        Copyright 2010 Intel Corporation
        Contributor: Pierre-Louis Bossart <pierre-louis.bossart@intel.com>

    PulseAudio is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License,
    or (at your option) any later version.

    PulseAudio is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with PulseAudio; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
    USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <math.h>

#include "echo-cancel.h"

#include <pulse/xmalloc.h>
#include <pulse/i18n.h>
#include <pulse/timeval.h>
#include <pulse/rtclock.h>

#include <pulsecore/atomic.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-error.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-error.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/ltdl-helper.h>

#include "module-echo-cancel-symdef.h"

PA_MODULE_AUTHOR("Wim Taymans");
PA_MODULE_DESCRIPTION("Echo Cancelation");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        _("source_name=<name for the source> "
          "source_properties=<properties for the source> "
          "source_master=<name of source to filter> "
          "sink_name=<name for the sink> "
          "sink_properties=<properties for the sink> "
          "sink_master=<name of sink to filter> "
          "adjust_time=<how often to readjust rates in s> "
          "format=<sample format> "
          "rate=<sample rate> "
          "channels=<number of channels> "
          "channel_map=<channel map> "
          "aec_method=<implementation to use> "
          "aec_args=<parameters for the AEC engine> "
          "save_aec=<save AEC data in /tmp> "
        ));

/* NOTE: Make sure the enum and ec_table are maintained in the correct order */
typedef enum {
    PA_ECHO_CANCELLER_INVALID = -1,
    PA_ECHO_CANCELLER_SPEEX = 0,
    PA_ECHO_CANCELLER_ADRIAN,
} pa_echo_canceller_method_t;

#define DEFAULT_ECHO_CANCELLER "speex"

static const pa_echo_canceller ec_table[] = {
    {
        /* Speex */
        .init                   = pa_speex_ec_init,
        .run                    = pa_speex_ec_run,
        .done                   = pa_speex_ec_done,
    },
    {
        /* Adrian Andre's NLMS implementation */
        .init                   = pa_adrian_ec_init,
        .run                    = pa_adrian_ec_run,
        .done                   = pa_adrian_ec_done,
    },
};

#define DEFAULT_ADJUST_TIME_USEC (1*PA_USEC_PER_SEC)
#define DEFAULT_SAVE_AEC 0

#define MEMBLOCKQ_MAXLENGTH (16*1024*1024)

/* This module creates a new (virtual) source and sink.
 *
 * The data sent to the new sink is kept in a memblockq before being
 * forwarded to the real sink_master.
 *
 * Data read from source_master is matched against the saved sink data and
 * echo canceled data is then pushed onto the new source.
 *
 * Both source and sink masters have their own threads to push/pull data
 * respectively. We however perform all our actions in the source IO thread.
 * To do this we send all played samples to the source IO thread where they
 * are then pushed into the memblockq.
 *
 * Alignment is performed in two steps:
 *
 * 1) when something happens that requires quick adjustement of the alignment of
 *    capture and playback samples, we perform a resync. This adjusts the
 *    position in the playback memblock to the requested sample. Quick
 *    adjustements include moving the playback samples before the capture
 *    samples (because else the echo canceler does not work) or when the
 *    playback pointer drifts too far away.
 *
 * 2) periodically check the difference between capture and playback. we use a
 *    low and high watermark for adjusting the alignment. playback should always
 *    be before capture and the difference should not be bigger than one frame
 *    size. We would ideally like to resample the sink_input but most driver
 *    don't give enough accuracy to be able to do that right now.
 */

struct snapshot {
    pa_usec_t sink_now;
    pa_usec_t sink_latency;
    size_t sink_delay;
    int64_t send_counter;

    pa_usec_t source_now;
    pa_usec_t source_latency;
    size_t source_delay;
    int64_t recv_counter;
    size_t rlen;
    size_t plen;
};

struct userdata {
    pa_core *core;
    pa_module *module;

    uint32_t save_aec;

    pa_echo_canceller *ec;
    uint32_t blocksize;

    pa_bool_t need_realign;

    /* to wakeup the source I/O thread */
    pa_bool_t in_push;
    pa_asyncmsgq *asyncmsgq;
    pa_rtpoll_item *rtpoll_item_read, *rtpoll_item_write;

    pa_source *source;
    pa_bool_t source_auto_desc;
    pa_source_output *source_output;
    pa_memblockq *source_memblockq; /* echo canceler needs fixed sized chunks */
    size_t source_skip;

    pa_sink *sink;
    pa_bool_t sink_auto_desc;
    pa_sink_input *sink_input;
    pa_memblockq *sink_memblockq;
    int64_t send_counter;          /* updated in sink IO thread */
    int64_t recv_counter;
    size_t sink_skip;

    pa_atomic_t request_resync;

    int active_mask;
    pa_time_event *time_event;
    pa_usec_t adjust_time;

    FILE *captured_file;
    FILE *played_file;
    FILE *canceled_file;
};

static void source_output_snapshot_within_thread(struct userdata *u, struct snapshot *snapshot);

static const char* const valid_modargs[] = {
    "source_name",
    "source_properties",
    "source_master",
    "sink_name",
    "sink_properties",
    "sink_master",
    "adjust_time",
    "format",
    "rate",
    "channels",
    "channel_map",
    "aec_method",
    "aec_args",
    "save_aec",
    NULL
};

enum {
    SOURCE_OUTPUT_MESSAGE_POST = PA_SOURCE_OUTPUT_MESSAGE_MAX,
    SOURCE_OUTPUT_MESSAGE_REWIND,
    SOURCE_OUTPUT_MESSAGE_LATENCY_SNAPSHOT,
    SOURCE_OUTPUT_MESSAGE_APPLY_DIFF_TIME
};

enum {
    SINK_INPUT_MESSAGE_LATENCY_SNAPSHOT
};

static int64_t calc_diff(struct userdata *u, struct snapshot *snapshot) {
    int64_t buffer, diff_time, buffer_latency;

    /* get the number of samples between capture and playback */
    if (snapshot->plen > snapshot->rlen)
        buffer = snapshot->plen - snapshot->rlen;
    else
        buffer = 0;

    buffer += snapshot->source_delay + snapshot->sink_delay;

    /* add the amount of samples not yet transfered to the source context */
    if (snapshot->recv_counter <= snapshot->send_counter)
        buffer += (int64_t) (snapshot->send_counter - snapshot->recv_counter);
    else
        buffer += PA_CLIP_SUB(buffer, (int64_t) (snapshot->recv_counter - snapshot->send_counter));

    /* convert to time */
    buffer_latency = pa_bytes_to_usec(buffer, &u->source_output->sample_spec);

    /* capture and playback samples are perfectly aligned when diff_time is 0 */
    diff_time = (snapshot->sink_now + snapshot->sink_latency - buffer_latency) -
          (snapshot->source_now - snapshot->source_latency);

    pa_log_debug("diff %lld (%lld - %lld + %lld) %lld %lld %lld %lld", (long long) diff_time,
        (long long) snapshot->sink_latency,
        (long long) buffer_latency, (long long) snapshot->source_latency,
        (long long) snapshot->source_delay, (long long) snapshot->sink_delay,
        (long long) (snapshot->send_counter - snapshot->recv_counter),
        (long long) (snapshot->sink_now - snapshot->source_now));

    return diff_time;
}

/* Called from main context */
static void time_callback(pa_mainloop_api *a, pa_time_event *e, const struct timeval *t, void *userdata) {
    struct userdata *u = userdata;
    uint32_t old_rate, base_rate, new_rate;
    int64_t diff_time;
    size_t fs;
    struct snapshot latency_snapshot;

    pa_assert(u);
    pa_assert(a);
    pa_assert(u->time_event == e);
    pa_assert_ctl_context();

    if (u->active_mask != 3)
        return;

    /* update our snapshots */
    pa_asyncmsgq_send(u->source_output->source->asyncmsgq, PA_MSGOBJECT(u->source_output), SOURCE_OUTPUT_MESSAGE_LATENCY_SNAPSHOT, &latency_snapshot, 0, NULL);
    pa_asyncmsgq_send(u->sink_input->sink->asyncmsgq, PA_MSGOBJECT(u->sink_input), SINK_INPUT_MESSAGE_LATENCY_SNAPSHOT, &latency_snapshot, 0, NULL);

    /* calculate drift between capture and playback */
    diff_time = calc_diff(u, &latency_snapshot);

    fs = pa_frame_size(&u->source_output->sample_spec);
    old_rate = u->sink_input->sample_spec.rate;
    base_rate = u->source_output->sample_spec.rate;

    if (diff_time < 0) {
        /* recording before playback, we need to adjust quickly. The echo
         * canceler does not work in this case. */
        pa_asyncmsgq_post(u->asyncmsgq, PA_MSGOBJECT(u->source_output), SOURCE_OUTPUT_MESSAGE_APPLY_DIFF_TIME,
            NULL, diff_time, NULL, NULL);
        //new_rate = base_rate - ((pa_usec_to_bytes (-diff_time, &u->source_output->sample_spec) / fs) * PA_USEC_PER_SEC) / u->adjust_time;
        new_rate = base_rate;
    }
    else {
        if (diff_time > 1000) {
            /* diff too big, quickly adjust */
            pa_asyncmsgq_post(u->asyncmsgq, PA_MSGOBJECT(u->source_output), SOURCE_OUTPUT_MESSAGE_APPLY_DIFF_TIME,
                NULL, diff_time, NULL, NULL);
        }

        /* recording behind playback, we need to slowly adjust the rate to match */
        //new_rate = base_rate + ((pa_usec_to_bytes (diff_time, &u->source_output->sample_spec) / fs) * PA_USEC_PER_SEC) / u->adjust_time;

        /* assume equal samplerates for now */
        new_rate = base_rate;
    }

    /* make sure we don't make too big adjustements because that sounds horrible */
    if (new_rate > base_rate * 1.1 || new_rate < base_rate * 0.9)
        new_rate = base_rate;

    if (new_rate != old_rate) {
        pa_log_info("Old rate %lu Hz, new rate %lu Hz", (unsigned long) old_rate, (unsigned long) new_rate);

        pa_sink_input_set_rate(u->sink_input, new_rate);
    }

    pa_core_rttime_restart(u->core, u->time_event, pa_rtclock_now() + u->adjust_time);
}

/* Called from source I/O thread context */
static int source_process_msg_cb(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SOURCE(o)->userdata;

    switch (code) {

        case PA_SOURCE_MESSAGE_GET_LATENCY:

            /* The source is _put() before the source output is, so let's
             * make sure we don't access it in that time. Also, the
             * source output is first shut down, the source second. */
            if (!PA_SOURCE_IS_LINKED(u->source->thread_info.state) ||
                !PA_SOURCE_OUTPUT_IS_LINKED(u->source_output->thread_info.state)) {
                *((pa_usec_t*) data) = 0;
                return 0;
            }

            *((pa_usec_t*) data) =

                /* Get the latency of the master source */
                pa_source_get_latency_within_thread(u->source_output->source) +
                /* Add the latency internal to our source output on top */
                pa_bytes_to_usec(pa_memblockq_get_length(u->source_output->thread_info.delay_memblockq), &u->source_output->source->sample_spec) +
                /* and the buffering we do on the source */
                pa_bytes_to_usec(u->blocksize, &u->source_output->source->sample_spec);

            return 0;

    }

    return pa_source_process_msg(o, code, data, offset, chunk);
}

/* Called from sink I/O thread context */
static int sink_process_msg_cb(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {

        case PA_SINK_MESSAGE_GET_LATENCY:

            /* The sink is _put() before the sink input is, so let's
             * make sure we don't access it in that time. Also, the
             * sink input is first shut down, the sink second. */
            if (!PA_SINK_IS_LINKED(u->sink->thread_info.state) ||
                !PA_SINK_INPUT_IS_LINKED(u->sink_input->thread_info.state)) {
                *((pa_usec_t*) data) = 0;
                return 0;
            }

            *((pa_usec_t*) data) =

                /* Get the latency of the master sink */
                pa_sink_get_latency_within_thread(u->sink_input->sink) +

                /* Add the latency internal to our sink input on top */
                pa_bytes_to_usec(pa_memblockq_get_length(u->sink_input->thread_info.render_memblockq), &u->sink_input->sink->sample_spec);

            return 0;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}


/* Called from main context */
static int source_set_state_cb(pa_source *s, pa_source_state_t state) {
    struct userdata *u;

    pa_source_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SOURCE_IS_LINKED(state) ||
        !PA_SOURCE_OUTPUT_IS_LINKED(pa_source_output_get_state(u->source_output)))
        return 0;

    pa_log_debug("Source state %d %d", state, u->active_mask);

    if (state == PA_SOURCE_RUNNING) {
        /* restart timer when both sink and source are active */
        u->active_mask |= 1;
        if (u->active_mask == 3)
            pa_core_rttime_restart(u->core, u->time_event, pa_rtclock_now() + u->adjust_time);

        pa_atomic_store (&u->request_resync, 1);
        pa_source_output_cork(u->source_output, FALSE);
    } else if (state == PA_SOURCE_SUSPENDED) {
        u->active_mask &= ~1;
        pa_source_output_cork(u->source_output, TRUE);
    }
    return 0;
}

/* Called from main context */
static int sink_set_state_cb(pa_sink *s, pa_sink_state_t state) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(state) ||
        !PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(u->sink_input)))
        return 0;

    pa_log_debug("Sink state %d %d", state, u->active_mask);

    if (state == PA_SINK_RUNNING) {
        /* restart timer when both sink and source are active */
        u->active_mask |= 2;
        if (u->active_mask == 3)
            pa_core_rttime_restart(u->core, u->time_event, pa_rtclock_now() + u->adjust_time);

        pa_atomic_store (&u->request_resync, 1);
        pa_sink_input_cork(u->sink_input, FALSE);
    } else if (state == PA_SINK_SUSPENDED) {
        u->active_mask &= ~2;
        pa_sink_input_cork(u->sink_input, TRUE);
    }
    return 0;
}

/* Called from I/O thread context */
static void source_update_requested_latency_cb(pa_source *s) {
    struct userdata *u;

    pa_source_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SOURCE_IS_LINKED(u->source->thread_info.state) ||
        !PA_SOURCE_OUTPUT_IS_LINKED(u->source_output->thread_info.state))
        return;

    pa_log_debug("Source update requested latency");

    /* Just hand this one over to the master source */
    pa_source_output_set_requested_latency_within_thread(
            u->source_output,
            pa_source_get_requested_latency_within_thread(s));
}

/* Called from I/O thread context */
static void sink_update_requested_latency_cb(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(u->sink->thread_info.state) ||
        !PA_SINK_INPUT_IS_LINKED(u->sink_input->thread_info.state))
        return;

    pa_log_debug("Sink update requested latency");

    /* Just hand this one over to the master sink */
    pa_sink_input_set_requested_latency_within_thread(
            u->sink_input,
            pa_sink_get_requested_latency_within_thread(s));
}

/* Called from I/O thread context */
static void sink_request_rewind_cb(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(u->sink->thread_info.state) ||
        !PA_SINK_INPUT_IS_LINKED(u->sink_input->thread_info.state))
        return;

    pa_log_debug("Sink request rewind %lld", (long long) s->thread_info.rewind_nbytes);

    /* Just hand this one over to the master sink */
    pa_sink_input_request_rewind(u->sink_input,
                                 s->thread_info.rewind_nbytes, TRUE, FALSE, FALSE);
}

/* Called from main context */
static void source_set_volume_cb(pa_source *s) {
    struct userdata *u;

    pa_source_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SOURCE_IS_LINKED(pa_source_get_state(s)) ||
        !PA_SOURCE_OUTPUT_IS_LINKED(pa_source_output_get_state(u->source_output)))
        return;

    /* FIXME, no volume control in source_output, set volume at the master */
    pa_source_set_volume(u->source_output->source, &s->volume, TRUE);
}

/* Called from main context */
static void sink_set_volume_cb(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(pa_sink_get_state(s)) ||
        !PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(u->sink_input)))
        return;

    pa_sink_input_set_volume(u->sink_input, &s->real_volume, s->save_volume, TRUE);
}

static void source_get_volume_cb(pa_source *s) {
    struct userdata *u;

    pa_source_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SOURCE_IS_LINKED(pa_source_get_state(s)) ||
        !PA_SOURCE_OUTPUT_IS_LINKED(pa_source_output_get_state(u->source_output)))
        return;

    /* FIXME, no volume control in source_output, get the info from the master */
    pa_source_get_volume(u->source_output->source, TRUE);

    if (pa_cvolume_equal(&s->volume,&u->source_output->source->volume))
        /* no change */
        return;

    s->volume = u->source_output->source->volume;
    pa_source_set_soft_volume(s, NULL);
}


/* Called from main context */
static void source_set_mute_cb(pa_source *s) {
    struct userdata *u;

    pa_source_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SOURCE_IS_LINKED(pa_source_get_state(s)) ||
        !PA_SOURCE_OUTPUT_IS_LINKED(pa_source_output_get_state(u->source_output)))
        return;

    /* FIXME, no volume control in source_output, set mute at the master */
    pa_source_set_mute(u->source_output->source, TRUE, TRUE);
}

/* Called from main context */
static void sink_set_mute_cb(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(pa_sink_get_state(s)) ||
        !PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(u->sink_input)))
        return;

    pa_sink_input_set_mute(u->sink_input, s->muted, s->save_muted);
}

/* Called from main context */
static void source_get_mute_cb(pa_source *s) {
    struct userdata *u;

    pa_source_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SOURCE_IS_LINKED(pa_source_get_state(s)) ||
        !PA_SOURCE_OUTPUT_IS_LINKED(pa_source_output_get_state(u->source_output)))
        return;

    /* FIXME, no volume control in source_output, get the info from the master */
    pa_source_get_mute(u->source_output->source, TRUE);
}

/* must be called from the input thread context */
static void apply_diff_time(struct userdata *u, int64_t diff_time) {
    int64_t diff;

    if (diff_time < 0) {
        diff = pa_usec_to_bytes (-diff_time, &u->source_output->sample_spec);

        if (diff > 0) {
            /* add some extra safety samples to compensate for jitter in the
             * timings */
            diff += 10 * pa_frame_size (&u->source_output->sample_spec);

            pa_log("Playback after capture (%lld), drop sink %lld", (long long) diff_time, (long long) diff);

            u->sink_skip = diff;
            u->source_skip = 0;
        }
    } else if (diff_time > 0) {
        diff = pa_usec_to_bytes (diff_time, &u->source_output->sample_spec);

        if (diff > 0) {
            pa_log("playback too far ahead (%lld), drop source %lld", (long long) diff_time, (long long) diff);

            u->source_skip = diff;
            u->sink_skip = 0;
        }
    }
}

/* must be called from the input thread */
static void do_resync(struct userdata *u) {
    int64_t diff_time;
    struct snapshot latency_snapshot;

    pa_log("Doing resync");

    /* update our snapshot */
    source_output_snapshot_within_thread(u, &latency_snapshot);
    pa_asyncmsgq_send(u->sink_input->sink->asyncmsgq, PA_MSGOBJECT(u->sink_input), SINK_INPUT_MESSAGE_LATENCY_SNAPSHOT, &latency_snapshot, 0, NULL);

    /* calculate drift between capture and playback */
    diff_time = calc_diff(u, &latency_snapshot);

    /* and adjust for the drift */
    apply_diff_time(u, diff_time);
}

/* Called from input thread context */
static void source_output_push_cb(pa_source_output *o, const pa_memchunk *chunk) {
    struct userdata *u;
    size_t rlen, plen;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert_se(u = o->userdata);

    if (!PA_SOURCE_OUTPUT_IS_LINKED(pa_source_output_get_state(u->source_output))) {
        pa_log("push when no link?");
        return;
    }

    /* handle queued messages */
    u->in_push = TRUE;
    while (pa_asyncmsgq_process_one(u->asyncmsgq) > 0)
        ;
    u->in_push = FALSE;

    if (pa_atomic_cmpxchg (&u->request_resync, 1, 0)) {
        do_resync (u);
    }

    pa_memblockq_push_align(u->source_memblockq, chunk);

    rlen = pa_memblockq_get_length(u->source_memblockq);
    plen = pa_memblockq_get_length(u->sink_memblockq);

    while (rlen >= u->blocksize) {
        pa_memchunk rchunk, pchunk;

        /* take fixed block from recorded samples */
        pa_memblockq_peek_fixed_size(u->source_memblockq, u->blocksize, &rchunk);

        if (plen > u->blocksize && u->source_skip == 0) {
            uint8_t *rdata, *pdata, *cdata;
            pa_memchunk cchunk;

            if (u->sink_skip) {
                size_t to_skip;

                if (u->sink_skip > plen)
                    to_skip = plen;
                else
                    to_skip = u->sink_skip;

                pa_memblockq_drop(u->sink_memblockq, to_skip);
                plen -= to_skip;

                u->sink_skip -= to_skip;
            }

            if (plen > u->blocksize && u->sink_skip == 0) {
                /* take fixed block from played samples */
                pa_memblockq_peek_fixed_size(u->sink_memblockq, u->blocksize, &pchunk);

                rdata = pa_memblock_acquire(rchunk.memblock);
                rdata += rchunk.index;
                pdata = pa_memblock_acquire(pchunk.memblock);
                pdata += pchunk.index;

                cchunk.index = 0;
                cchunk.length = u->blocksize;
                cchunk.memblock = pa_memblock_new(u->source->core->mempool, cchunk.length);
                cdata = pa_memblock_acquire(cchunk.memblock);

                /* perform echo cancelation */
                u->ec->run(u->ec, rdata, pdata, cdata);

                if (u->save_aec) {
                    if (u->captured_file)
                        fwrite(rdata, 1, u->blocksize, u->captured_file);
                    if (u->played_file)
                        fwrite(pdata, 1, u->blocksize, u->played_file);
                    if (u->canceled_file)
                        fwrite(cdata, 1, u->blocksize, u->canceled_file);
                    pa_log_debug("AEC frame saved.");
                }

                pa_memblock_release(cchunk.memblock);
                pa_memblock_release(pchunk.memblock);
                pa_memblock_release(rchunk.memblock);

                /* drop consumed sink samples */
                pa_memblockq_drop(u->sink_memblockq, u->blocksize);
                pa_memblock_unref(pchunk.memblock);

                pa_memblock_unref(rchunk.memblock);
                /* the filtered samples now become the samples from our
                 * source */
                rchunk = cchunk;

                plen -= u->blocksize;
            }
        }

        /* forward the (echo-canceled) data to the virtual source */
        pa_source_post(u->source, &rchunk);
        pa_memblock_unref(rchunk.memblock);

        pa_memblockq_drop(u->source_memblockq, u->blocksize);
        rlen -= u->blocksize;

        if (u->source_skip) {
            if (u->source_skip > u->blocksize) {
                u->source_skip -= u->blocksize;
            }
            else {
                u->sink_skip += (u->blocksize - u->source_skip);
                u->source_skip = 0;
            }
        }
    }
}

/* Called from I/O thread context */
static int sink_input_pop_cb(pa_sink_input *i, size_t nbytes, pa_memchunk *chunk) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert(chunk);
    pa_assert_se(u = i->userdata);

    if (u->sink->thread_info.rewind_requested)
        pa_sink_process_rewind(u->sink, 0);

    pa_sink_render_full(u->sink, nbytes, chunk);

    if (i->thread_info.underrun_for > 0) {
        pa_log_debug("Handling end of underrun.");
        pa_atomic_store (&u->request_resync, 1);
    }

    /* let source thread handle the chunk. pass the sample count as well so that
     * the source IO thread can update the right variables. */
    pa_asyncmsgq_post(u->asyncmsgq, PA_MSGOBJECT(u->source_output), SOURCE_OUTPUT_MESSAGE_POST,
        NULL, 0, chunk, NULL);
    u->send_counter += chunk->length;

    return 0;
}

/* Called from input thread context */
static void source_output_process_rewind_cb(pa_source_output *o, size_t nbytes) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert_se(u = o->userdata);

    pa_source_process_rewind(u->source, nbytes);

    /* go back on read side, we need to use older sink data for this */
    pa_memblockq_rewind(u->sink_memblockq, nbytes);

    /* manipulate write index */
    pa_memblockq_seek(u->source_memblockq, -nbytes, PA_SEEK_RELATIVE, TRUE);

    pa_log_debug("Source rewind (%lld) %lld", (long long) nbytes,
        (long long) pa_memblockq_get_length (u->source_memblockq));
}

/* Called from I/O thread context */
static void sink_input_process_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_log_debug("Sink process rewind %lld", (long long) nbytes);

    pa_sink_process_rewind(u->sink, nbytes);

    pa_asyncmsgq_post(u->asyncmsgq, PA_MSGOBJECT(u->source_output), SOURCE_OUTPUT_MESSAGE_REWIND, NULL, (int64_t) nbytes, NULL, NULL);
    u->send_counter -= nbytes;
}

static void source_output_snapshot_within_thread(struct userdata *u, struct snapshot *snapshot) {
    size_t delay, rlen, plen;
    pa_usec_t now, latency;

    now = pa_rtclock_now();
    latency = pa_source_get_latency_within_thread(u->source_output->source);
    delay = pa_memblockq_get_length(u->source_output->thread_info.delay_memblockq);

    delay = (u->source_output->thread_info.resampler ? pa_resampler_request(u->source_output->thread_info.resampler, delay) : delay);
    rlen = pa_memblockq_get_length(u->source_memblockq);
    plen = pa_memblockq_get_length(u->sink_memblockq);

    snapshot->source_now = now;
    snapshot->source_latency = latency;
    snapshot->source_delay = delay;
    snapshot->recv_counter = u->recv_counter;
    snapshot->rlen = rlen + u->sink_skip;
    snapshot->plen = plen + u->source_skip;
}


/* Called from output thread context */
static int source_output_process_msg_cb(pa_msgobject *obj, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SOURCE_OUTPUT(obj)->userdata;

    switch (code) {

        case SOURCE_OUTPUT_MESSAGE_POST:

            pa_source_output_assert_io_context(u->source_output);

            if (PA_SOURCE_IS_OPENED(u->source_output->source->thread_info.state))
                pa_memblockq_push_align(u->sink_memblockq, chunk);
            else
                pa_memblockq_flush_write(u->sink_memblockq, TRUE);

            u->recv_counter += (int64_t) chunk->length;

            return 0;

        case SOURCE_OUTPUT_MESSAGE_REWIND:
            pa_source_output_assert_io_context(u->source_output);

            /* manipulate write index, never go past what we have */
            if (PA_SOURCE_IS_OPENED(u->source_output->source->thread_info.state))
                pa_memblockq_seek(u->sink_memblockq, -offset, PA_SEEK_RELATIVE, TRUE);
            else
                pa_memblockq_flush_write(u->sink_memblockq, TRUE);

            pa_log_debug("Sink rewind (%lld)", (long long) offset);

            u->recv_counter -= offset;

            return 0;

        case SOURCE_OUTPUT_MESSAGE_LATENCY_SNAPSHOT: {
            struct snapshot *snapshot = (struct snapshot *) data;

            source_output_snapshot_within_thread(u, snapshot);
            return 0;
        }

        case SOURCE_OUTPUT_MESSAGE_APPLY_DIFF_TIME:
            apply_diff_time(u, offset);
            return 0;

    }

    return pa_source_output_process_msg(obj, code, data, offset, chunk);
}

static int sink_input_process_msg_cb(pa_msgobject *obj, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK_INPUT(obj)->userdata;

    switch (code) {

        case SINK_INPUT_MESSAGE_LATENCY_SNAPSHOT: {
            size_t delay;
            pa_usec_t now, latency;
            struct snapshot *snapshot = (struct snapshot *) data;

            pa_sink_input_assert_io_context(u->sink_input);

            now = pa_rtclock_now();
            latency = pa_sink_get_latency_within_thread(u->sink_input->sink);
            delay = pa_memblockq_get_length(u->sink_input->thread_info.render_memblockq);

            delay = (u->sink_input->thread_info.resampler ? pa_resampler_request(u->sink_input->thread_info.resampler, delay) : delay);

            snapshot->sink_now = now;
            snapshot->sink_latency = latency;
            snapshot->sink_delay = delay;
            snapshot->send_counter = u->send_counter;
            return 0;
        }
    }

    return pa_sink_input_process_msg(obj, code, data, offset, chunk);
}

/* Called from I/O thread context */
static void sink_input_update_max_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_log_debug("Sink input update max rewind %lld", (long long) nbytes);

    pa_memblockq_set_maxrewind (u->sink_memblockq, nbytes);
    pa_sink_set_max_rewind_within_thread(u->sink, nbytes);
}

/* Called from I/O thread context */
static void source_output_update_max_rewind_cb(pa_source_output *o, size_t nbytes) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    pa_log_debug("Source output update max rewind %lld", (long long) nbytes);

    pa_source_set_max_rewind_within_thread(u->source, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_max_request_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_log_debug("Sink input update max request %lld", (long long) nbytes);

    pa_sink_set_max_request_within_thread(u->sink, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_sink_requested_latency_cb(pa_sink_input *i) {
    struct userdata *u;
    pa_usec_t latency;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    latency = pa_sink_get_requested_latency_within_thread(i->sink);

    pa_log_debug("Sink input update requested latency %lld", (long long) latency);
}

/* Called from I/O thread context */
static void source_output_update_source_requested_latency_cb(pa_source_output *o) {
    struct userdata *u;
    pa_usec_t latency;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    latency = pa_source_get_requested_latency_within_thread(o->source);

    pa_log_debug("source output update requested latency %lld", (long long) latency);
}

/* Called from I/O thread context */
static void sink_input_update_sink_latency_range_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_log_debug("Sink input update latency range %lld %lld",
        (long long) i->sink->thread_info.min_latency,
        (long long) i->sink->thread_info.max_latency);

    pa_sink_set_latency_range_within_thread(u->sink, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);
}

/* Called from I/O thread context */
static void source_output_update_source_latency_range_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    pa_log_debug("Source output update latency range %lld %lld",
        (long long) o->source->thread_info.min_latency,
        (long long) o->source->thread_info.max_latency);

    pa_source_set_latency_range_within_thread(u->source, o->source->thread_info.min_latency, o->source->thread_info.max_latency);
}

/* Called from I/O thread context */
static void sink_input_update_sink_fixed_latency_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_log_debug("Sink input update fixed latency %lld",
        (long long) i->sink->thread_info.fixed_latency);

    pa_sink_set_fixed_latency_within_thread(u->sink, i->sink->thread_info.fixed_latency);
}

/* Called from I/O thread context */
static void source_output_update_source_fixed_latency_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    pa_log_debug("Source output update fixed latency %lld",
        (long long) o->source->thread_info.fixed_latency);

    pa_source_set_fixed_latency_within_thread(u->source, o->source->thread_info.fixed_latency);
}

/* Called from output thread context */
static void source_output_attach_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert_se(u = o->userdata);

    pa_source_set_rtpoll(u->source, o->source->thread_info.rtpoll);
    pa_source_set_latency_range_within_thread(u->source, o->source->thread_info.min_latency, o->source->thread_info.max_latency);
    pa_source_set_fixed_latency_within_thread(u->source, o->source->thread_info.fixed_latency);
    pa_source_set_max_rewind_within_thread(u->source, pa_source_output_get_max_rewind(o));

    pa_log_debug("Source output %p attach", o);

    pa_source_attach_within_thread(u->source);

    u->rtpoll_item_read = pa_rtpoll_item_new_asyncmsgq_read(
            o->source->thread_info.rtpoll,
            PA_RTPOLL_LATE,
            u->asyncmsgq);
}

/* Called from I/O thread context */
static void sink_input_attach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_set_rtpoll(u->sink, i->sink->thread_info.rtpoll);
    pa_sink_set_latency_range_within_thread(u->sink, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);

    /* (8.1) IF YOU NEED A FIXED BLOCK SIZE ADD THE LATENCY FOR ONE
     * BLOCK MINUS ONE SAMPLE HERE. SEE (7) */
    pa_sink_set_fixed_latency_within_thread(u->sink, i->sink->thread_info.fixed_latency);

    /* (8.2) IF YOU NEED A FIXED BLOCK SIZE ROUND
     * pa_sink_input_get_max_request(i) UP TO MULTIPLES OF IT
     * HERE. SEE (6) */
    pa_sink_set_max_request_within_thread(u->sink, pa_sink_input_get_max_request(i));
    pa_sink_set_max_rewind_within_thread(u->sink, pa_sink_input_get_max_rewind(i));

    pa_log_debug("Sink input %p attach", i);

    u->rtpoll_item_write = pa_rtpoll_item_new_asyncmsgq_write(
            i->sink->thread_info.rtpoll,
            PA_RTPOLL_LATE,
            u->asyncmsgq);

    pa_sink_attach_within_thread(u->sink);
}


/* Called from output thread context */
static void source_output_detach_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert_se(u = o->userdata);

    pa_source_detach_within_thread(u->source);
    pa_source_set_rtpoll(u->source, NULL);

    pa_log_debug("Source output %p detach", o);

    if (u->rtpoll_item_read) {
        pa_rtpoll_item_free(u->rtpoll_item_read);
        u->rtpoll_item_read = NULL;
    }
}

/* Called from I/O thread context */
static void sink_input_detach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_detach_within_thread(u->sink);

    pa_sink_set_rtpoll(u->sink, NULL);

    pa_log_debug("Sink input %p detach", i);

    if (u->rtpoll_item_write) {
        pa_rtpoll_item_free(u->rtpoll_item_write);
        u->rtpoll_item_write = NULL;
    }
}

/* Called from output thread context */
static void source_output_state_change_cb(pa_source_output *o, pa_source_output_state_t state) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert_se(u = o->userdata);

    pa_log_debug("Source output %p state %d", o, state);
}

/* Called from IO thread context */
static void sink_input_state_change_cb(pa_sink_input *i, pa_sink_input_state_t state) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_log_debug("Sink input %p state %d", i, state);

    /* If we are added for the first time, ask for a rewinding so that
     * we are heard right-away. */
    if (PA_SINK_INPUT_IS_LINKED(state) &&
        i->thread_info.state == PA_SINK_INPUT_INIT) {
        pa_log_debug("Requesting rewind due to state change.");
        pa_sink_input_request_rewind(i, 0, FALSE, TRUE, TRUE);
    }
}

/* Called from main thread */
static void source_output_kill_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert_se(u = o->userdata);

    /* The order here matters! We first kill the source output, followed
     * by the source. That means the source callbacks must be protected
     * against an unconnected source output! */
    pa_source_output_unlink(u->source_output);
    pa_source_unlink(u->source);

    pa_source_output_unref(u->source_output);
    u->source_output = NULL;

    pa_source_unref(u->source);
    u->source = NULL;

    pa_log_debug("Source output kill %p", o);

    pa_module_unload_request(u->module, TRUE);
}

/* Called from main context */
static void sink_input_kill_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    /* The order here matters! We first kill the sink input, followed
     * by the sink. That means the sink callbacks must be protected
     * against an unconnected sink input! */
    pa_sink_input_unlink(u->sink_input);
    pa_sink_unlink(u->sink);

    pa_sink_input_unref(u->sink_input);
    u->sink_input = NULL;

    pa_sink_unref(u->sink);
    u->sink = NULL;

    pa_log_debug("Sink input kill %p", i);

    pa_module_unload_request(u->module, TRUE);
}

/* Called from main thread */
static pa_bool_t source_output_may_move_to_cb(pa_source_output *o, pa_source *dest) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert_se(u = o->userdata);

    return (u->source != dest) && (u->sink != dest->monitor_of);
}

/* Called from main context */
static pa_bool_t sink_input_may_move_to_cb(pa_sink_input *i, pa_sink *dest) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    return u->sink != dest;
}

/* Called from main thread */
static void source_output_moving_cb(pa_source_output *o, pa_source *dest) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert_se(u = o->userdata);

    if (dest) {
        pa_source_set_asyncmsgq(u->source, dest->asyncmsgq);
        pa_source_update_flags(u->source, PA_SOURCE_LATENCY|PA_SOURCE_DYNAMIC_LATENCY, dest->flags);
    } else
        pa_source_set_asyncmsgq(u->source, NULL);

    if (u->source_auto_desc && dest) {
        const char *z;
        pa_proplist *pl;

        pl = pa_proplist_new();
        z = pa_proplist_gets(dest->proplist, PA_PROP_DEVICE_DESCRIPTION);
        pa_proplist_setf(pl, PA_PROP_DEVICE_DESCRIPTION, "Echo-Cancel Source %s on %s",
                         pa_proplist_gets(u->source->proplist, "device.echo-cancel.name"), z ? z : dest->name);

        pa_source_update_proplist(u->source, PA_UPDATE_REPLACE, pl);
        pa_proplist_free(pl);
    }
}

/* Called from main context */
static void sink_input_moving_cb(pa_sink_input *i, pa_sink *dest) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (dest) {
        pa_sink_set_asyncmsgq(u->sink, dest->asyncmsgq);
        pa_sink_update_flags(u->sink, PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY, dest->flags);
    } else
        pa_sink_set_asyncmsgq(u->sink, NULL);

    if (u->sink_auto_desc && dest) {
        const char *z;
        pa_proplist *pl;

        pl = pa_proplist_new();
        z = pa_proplist_gets(dest->proplist, PA_PROP_DEVICE_DESCRIPTION);
        pa_proplist_setf(pl, PA_PROP_DEVICE_DESCRIPTION, "Echo-Cancel Sink %s on %s",
                         pa_proplist_gets(u->sink->proplist, "device.echo-cancel.name"), z ? z : dest->name);

        pa_sink_update_proplist(u->sink, PA_UPDATE_REPLACE, pl);
        pa_proplist_free(pl);
    }
}

/* Called from main context */
static void sink_input_volume_changed_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_volume_changed(u->sink, &i->volume);
}

/* Called from main context */
static void sink_input_mute_changed_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_mute_changed(u->sink, i->muted);
}

static pa_echo_canceller_method_t get_ec_method_from_string(const char *method)
{
    if (strcmp(method, "speex") == 0)
        return PA_ECHO_CANCELLER_SPEEX;
    else if (strcmp(method, "adrian") == 0)
        return PA_ECHO_CANCELLER_ADRIAN;
    else
        return PA_ECHO_CANCELLER_INVALID;
}

int pa__init(pa_module*m) {
    struct userdata *u;
    pa_sample_spec source_ss, sink_ss;
    pa_channel_map source_map, sink_map;
    pa_modargs *ma;
    pa_source *source_master=NULL;
    pa_sink *sink_master=NULL;
    pa_source_output_new_data source_output_data;
    pa_sink_input_new_data sink_input_data;
    pa_source_new_data source_data;
    pa_sink_new_data sink_data;
    pa_memchunk silence;
    pa_echo_canceller_method_t ec_method;
    uint32_t adjust_time_sec;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    if (!(source_master = pa_namereg_get(m->core, pa_modargs_get_value(ma, "source_master", NULL), PA_NAMEREG_SOURCE))) {
        pa_log("Master source not found");
        goto fail;
    }
    pa_assert(source_master);

    if (!(sink_master = pa_namereg_get(m->core, pa_modargs_get_value(ma, "sink_master", NULL), PA_NAMEREG_SINK))) {
        pa_log("Master sink not found");
        goto fail;
    }
    pa_assert(sink_master);

    source_ss = source_master->sample_spec;
    source_map = source_master->channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &source_ss, &source_map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    sink_ss = sink_master->sample_spec;
    sink_map = sink_master->channel_map;

    u = pa_xnew0(struct userdata, 1);
    if (!u) {
        pa_log("Failed to alloc userdata");
        goto fail;
    }
    u->core = m->core;
    u->module = m;
    m->userdata = u;

    u->ec = pa_xnew0(pa_echo_canceller, 1);
    if (!u->ec) {
        pa_log("Failed to alloc echo canceller");
        goto fail;
    }

    if ((ec_method = get_ec_method_from_string(pa_modargs_get_value(ma, "aec_method", DEFAULT_ECHO_CANCELLER))) < 0) {
        pa_log("Invalid echo canceller implementation");
        goto fail;
    }

    u->ec->init = ec_table[ec_method].init;
    u->ec->run = ec_table[ec_method].run;
    u->ec->done = ec_table[ec_method].done;

    adjust_time_sec = DEFAULT_ADJUST_TIME_USEC / PA_USEC_PER_SEC;
    if (pa_modargs_get_value_u32(ma, "adjust_time", &adjust_time_sec) < 0) {
        pa_log("Failed to parse adjust_time value");
        goto fail;
    }

    if (adjust_time_sec != DEFAULT_ADJUST_TIME_USEC / PA_USEC_PER_SEC)
        u->adjust_time = adjust_time_sec * PA_USEC_PER_SEC;
    else
        u->adjust_time = DEFAULT_ADJUST_TIME_USEC;

    u->save_aec = DEFAULT_SAVE_AEC;
    if (pa_modargs_get_value_u32(ma, "save_aec", &u->save_aec) < 0) {
        pa_log("Failed to parse save_aec value");
        goto fail;
    }

    u->asyncmsgq = pa_asyncmsgq_new(0);
    u->need_realign = TRUE;
    if (u->ec->init) {
        if (!u->ec->init(u->core, u->ec, &source_ss, &source_map, &sink_ss, &sink_map, &u->blocksize, pa_modargs_get_value(ma, "aec_args", NULL))) {
            pa_log("Failed to init AEC engine");
            goto fail;
        }
    }

    /* Create source */
    pa_source_new_data_init(&source_data);
    source_data.driver = __FILE__;
    source_data.module = m;
    if (!(source_data.name = pa_xstrdup(pa_modargs_get_value(ma, "source_name", NULL))))
        source_data.name = pa_sprintf_malloc("%s.echo-cancel", source_master->name);
    pa_source_new_data_set_sample_spec(&source_data, &source_ss);
    pa_source_new_data_set_channel_map(&source_data, &source_map);
    pa_proplist_sets(source_data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, source_master->name);
    pa_proplist_sets(source_data.proplist, PA_PROP_DEVICE_CLASS, "filter");
    pa_proplist_sets(source_data.proplist, PA_PROP_DEVICE_INTENDED_ROLES, "phone");
    pa_proplist_sets(source_data.proplist, "device.echo-cancel.name", source_data.name);

    if (pa_modargs_get_proplist(ma, "source_properties", source_data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_source_new_data_done(&source_data);
        goto fail;
    }

    if ((u->source_auto_desc = !pa_proplist_contains(source_data.proplist, PA_PROP_DEVICE_DESCRIPTION))) {
        const char *z;

        z = pa_proplist_gets(source_master->proplist, PA_PROP_DEVICE_DESCRIPTION);
        pa_proplist_setf(source_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Echo-Cancel Source %s on %s", source_data.name, z ? z : source_master->name);
    }

    u->source = pa_source_new(m->core, &source_data,
                          PA_SOURCE_HW_MUTE_CTRL|PA_SOURCE_HW_VOLUME_CTRL|PA_SOURCE_DECIBEL_VOLUME|
                          (source_master->flags & (PA_SOURCE_LATENCY|PA_SOURCE_DYNAMIC_LATENCY)));
    pa_source_new_data_done(&source_data);

    if (!u->source) {
        pa_log("Failed to create source.");
        goto fail;
    }

    u->source->parent.process_msg = source_process_msg_cb;
    u->source->set_state = source_set_state_cb;
    u->source->update_requested_latency = source_update_requested_latency_cb;
    u->source->set_volume = source_set_volume_cb;
    u->source->set_mute = source_set_mute_cb;
    u->source->get_volume = source_get_volume_cb;
    u->source->get_mute = source_get_mute_cb;
    u->source->userdata = u;

    pa_source_set_asyncmsgq(u->source, source_master->asyncmsgq);

    /* Create sink */
    pa_sink_new_data_init(&sink_data);
    sink_data.driver = __FILE__;
    sink_data.module = m;
    if (!(sink_data.name = pa_xstrdup(pa_modargs_get_value(ma, "sink_name", NULL))))
        sink_data.name = pa_sprintf_malloc("%s.echo-cancel", sink_master->name);
    pa_sink_new_data_set_sample_spec(&sink_data, &sink_ss);
    pa_sink_new_data_set_channel_map(&sink_data, &sink_map);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, sink_master->name);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_CLASS, "filter");
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_INTENDED_ROLES, "phone");
    pa_proplist_sets(sink_data.proplist, "device.echo-cancel.name", sink_data.name);

    if (pa_modargs_get_proplist(ma, "sink_properties", sink_data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_sink_new_data_done(&sink_data);
        goto fail;
    }

    if ((u->sink_auto_desc = !pa_proplist_contains(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION))) {
        const char *z;

        z = pa_proplist_gets(sink_master->proplist, PA_PROP_DEVICE_DESCRIPTION);
        pa_proplist_setf(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Echo-Cancel Sink %s on %s", sink_data.name, z ? z : sink_master->name);
    }

    u->sink = pa_sink_new(m->core, &sink_data,
                          PA_SINK_HW_MUTE_CTRL|PA_SINK_HW_VOLUME_CTRL|PA_SINK_DECIBEL_VOLUME|
                          (sink_master->flags & (PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY)));
    pa_sink_new_data_done(&sink_data);

    if (!u->sink) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg_cb;
    u->sink->set_state = sink_set_state_cb;
    u->sink->update_requested_latency = sink_update_requested_latency_cb;
    u->sink->request_rewind = sink_request_rewind_cb;
    u->sink->set_volume = sink_set_volume_cb;
    u->sink->set_mute = sink_set_mute_cb;
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, sink_master->asyncmsgq);

    /* Create source output */
    pa_source_output_new_data_init(&source_output_data);
    source_output_data.driver = __FILE__;
    source_output_data.module = m;
    source_output_data.source = source_master;
    /* FIXME
       source_output_data.flags = PA_SOURCE_OUTPUT_DONT_INHIBIT_AUTO_SUSPEND; */

    pa_proplist_sets(source_output_data.proplist, PA_PROP_MEDIA_NAME, "Echo-Cancel Source Stream");
    pa_proplist_sets(source_output_data.proplist, PA_PROP_MEDIA_ROLE, "filter");
    pa_source_output_new_data_set_sample_spec(&source_output_data, &source_ss);
    pa_source_output_new_data_set_channel_map(&source_output_data, &source_map);

    pa_source_output_new(&u->source_output, m->core, &source_output_data);
    pa_source_output_new_data_done(&source_output_data);

    if (!u->source_output)
        goto fail;

    u->source_output->parent.process_msg = source_output_process_msg_cb;
    u->source_output->push = source_output_push_cb;
    u->source_output->process_rewind = source_output_process_rewind_cb;
    u->source_output->update_max_rewind = source_output_update_max_rewind_cb;
    u->source_output->update_source_requested_latency = source_output_update_source_requested_latency_cb;
    u->source_output->update_source_latency_range = source_output_update_source_latency_range_cb;
    u->source_output->update_source_fixed_latency = source_output_update_source_fixed_latency_cb;
    u->source_output->kill = source_output_kill_cb;
    u->source_output->attach = source_output_attach_cb;
    u->source_output->detach = source_output_detach_cb;
    u->source_output->state_change = source_output_state_change_cb;
    u->source_output->may_move_to = source_output_may_move_to_cb;
    u->source_output->moving = source_output_moving_cb;
    u->source_output->userdata = u;

    /* Create sink input */
    pa_sink_input_new_data_init(&sink_input_data);
    sink_input_data.driver = __FILE__;
    sink_input_data.module = m;
    sink_input_data.sink = sink_master;
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_NAME, "Echo-Cancel Sink Stream");
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_ROLE, "filter");
    pa_sink_input_new_data_set_sample_spec(&sink_input_data, &sink_ss);
    pa_sink_input_new_data_set_channel_map(&sink_input_data, &sink_map);
    sink_input_data.flags = PA_SINK_INPUT_VARIABLE_RATE;

    pa_sink_input_new(&u->sink_input, m->core, &sink_input_data);
    pa_sink_input_new_data_done(&sink_input_data);

    if (!u->sink_input)
        goto fail;

    u->sink_input->parent.process_msg = sink_input_process_msg_cb;
    u->sink_input->pop = sink_input_pop_cb;
    u->sink_input->process_rewind = sink_input_process_rewind_cb;
    u->sink_input->update_max_rewind = sink_input_update_max_rewind_cb;
    u->sink_input->update_max_request = sink_input_update_max_request_cb;
    u->sink_input->update_sink_requested_latency = sink_input_update_sink_requested_latency_cb;
    u->sink_input->update_sink_latency_range = sink_input_update_sink_latency_range_cb;
    u->sink_input->update_sink_fixed_latency = sink_input_update_sink_fixed_latency_cb;
    u->sink_input->kill = sink_input_kill_cb;
    u->sink_input->attach = sink_input_attach_cb;
    u->sink_input->detach = sink_input_detach_cb;
    u->sink_input->state_change = sink_input_state_change_cb;
    u->sink_input->may_move_to = sink_input_may_move_to_cb;
    u->sink_input->moving = sink_input_moving_cb;
    u->sink_input->volume_changed = sink_input_volume_changed_cb;
    u->sink_input->mute_changed = sink_input_mute_changed_cb;
    u->sink_input->userdata = u;

    pa_sink_input_get_silence(u->sink_input, &silence);

    u->source_memblockq = pa_memblockq_new(0, MEMBLOCKQ_MAXLENGTH, 0,
        pa_frame_size(&source_ss), 1, 1, 0, &silence);
    u->sink_memblockq = pa_memblockq_new(0, MEMBLOCKQ_MAXLENGTH, 0,
        pa_frame_size(&sink_ss), 1, 1, 0, &silence);

    pa_memblock_unref(silence.memblock);

    if (!u->source_memblockq || !u->sink_memblockq) {
        pa_log("Failed to create memblockq.");
        goto fail;
    }

    /* our source and sink are not suspended when we create them */
    u->active_mask = 3;

    if (u->adjust_time > 0)
        u->time_event = pa_core_rttime_new(m->core, pa_rtclock_now() + u->adjust_time, time_callback, u);

    if (u->save_aec) {
        pa_log("Creating AEC files in /tmp");
        u->captured_file = fopen("/tmp/aec_rec.sw", "wb");
        if (u->captured_file == NULL)
            perror ("fopen failed");
        u->played_file = fopen("/tmp/aec_play.sw", "wb");
        if (u->played_file == NULL)
            perror ("fopen failed");
        u->canceled_file = fopen("/tmp/aec_out.sw", "wb");
        if (u->canceled_file == NULL)
            perror ("fopen failed");
    }

    pa_sink_put(u->sink);
    pa_source_put(u->source);

    pa_sink_input_put(u->sink_input);
    pa_source_output_put(u->source_output);

    pa_modargs_free(ma);

    return 0;

 fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

int pa__get_n_used(pa_module *m) {
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    return pa_sink_linked_by(u->sink) +  pa_source_linked_by(u->source);
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    /* See comments in source_output_kill_cb() above regarding
     * destruction order! */

    if (u->time_event)
        u->core->mainloop->time_free(u->time_event);

    if (u->source_output)
        pa_source_output_unlink(u->source_output);
    if (u->sink_input)
        pa_sink_input_unlink(u->sink_input);

    if (u->source)
        pa_source_unlink(u->source);
    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->source_output)
        pa_source_output_unref(u->source_output);
    if (u->sink_input)
        pa_sink_input_unref(u->sink_input);

    if (u->source)
        pa_source_unref(u->source);
    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->source_memblockq)
        pa_memblockq_free(u->source_memblockq);
    if (u->sink_memblockq)
        pa_memblockq_free(u->sink_memblockq);

    if (u->ec) {
        if (u->ec->done)
            u->ec->done(u->ec);

        pa_xfree(u->ec);
    }

    if (u->asyncmsgq)
        pa_asyncmsgq_unref(u->asyncmsgq);

    pa_xfree(u);
}
