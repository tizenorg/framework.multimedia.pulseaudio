/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

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

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulse/rtclock.h>

#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>
#include <pulsecore/strbuf.h>
#include "module-suspend-on-idle-symdef.h"

#define USE_AUDIO_PM /* For ymu831 */
#ifdef USE_AUDIO_PM
#include <pulsecore/protocol-native.h>
#include <pulsecore/pstream-util.h>
#include <alsa/ascenario.h>
#include <vconf.h>
#include <pulsecore/mutex.h>
#endif /* USE_AUDIO_PM */

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("When a sink/source is idle for too long, suspend it");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);

#define USE_PM_LOCK /* Enable as default */
#ifdef USE_PM_LOCK
#include "pm-util.h"

typedef struct pa_pm_list pa_pm_list;

struct pa_pm_list {
    uint32_t index;
    pa_bool_t is_sink;
    PA_LLIST_FIELDS(pa_pm_list);
};

#endif


static const char* const valid_modargs[] = {
    "timeout",
#ifdef USE_AUDIO_PM
    "use_audio_pm",
#endif /* USE_AUDIO_PM */
    NULL,
};

struct userdata {
    pa_core *core;
    pa_usec_t timeout;
    pa_hashmap *device_infos;
    pa_hook_slot
        *sink_new_slot,
        *source_new_slot,
        *sink_unlink_slot,
        *source_unlink_slot,
        *sink_state_changed_slot,
        *source_state_changed_slot;

    pa_hook_slot
        *sink_input_new_slot,
        *source_output_new_slot,
        *sink_input_unlink_slot,
        *source_output_unlink_slot,
        *sink_input_move_start_slot,
        *source_output_move_start_slot,
        *sink_input_move_finish_slot,
        *source_output_move_finish_slot,
        *sink_input_state_changed_slot,
        *source_output_state_changed_slot;
#ifdef USE_PM_LOCK
    pa_mutex* pm_mutex;
    PA_LLIST_HEAD(pa_pm_list, pm_list);
    pa_bool_t is_pm_locked;
#endif /* USE_PM_LOCK */
#ifdef USE_AUDIO_PM
    pa_native_protocol *protocol;
    pa_bool_t use_audio_pm;
    struct snd_scenario *scn;
    pa_bool_t is_sink_suspended;
    pa_bool_t is_source_suspended;
    pa_bool_t is_on_call;
    pa_mutex* m;
#endif /* USE_AUDIO_PM */
};

struct device_info {
    struct userdata *userdata;
    pa_sink *sink;
    pa_source *source;
    pa_usec_t last_use;
    pa_time_event *time_event;
};


#ifdef USE_PM_LOCK

enum {
    PM_SUSPEND = 0,
    PM_RESUME
};

enum {
    PM_SOURCE = 0,
    PM_SINK
};

#define GET_STR(s) ((s)? "sink":"source")
#define USE_DEVICE_DEFAULT_TIMEOUT         -1

static void _pm_list_add_if_not_exist(struct userdata *u, int is_sink, uint32_t index_to_add)
{
    struct pa_pm_list *item_info = NULL;
    struct pa_pm_list *item_info_n = NULL;

    /* Search if exists */
    PA_LLIST_FOREACH_SAFE(item_info, item_info_n, u->pm_list) {
        if (item_info->is_sink == is_sink && item_info->index == index_to_add) {
        pa_log_debug_verbose("[PM] Already index (%s:%d) exists on list[%p], return",
                GET_STR(is_sink), index_to_add, u->pm_list);
        return;
        }
    }

    /* Add new index */
    item_info = pa_xnew0(pa_pm_list, 1);
    item_info->is_sink = is_sink;
    item_info->index = index_to_add;

    PA_LLIST_PREPEND(pa_pm_list, u->pm_list, item_info);

    pa_log_debug_verbose("[PM] Added (%s:%d) to list[%p]", GET_STR(is_sink), index_to_add, u->pm_list);
}

static void _pm_list_remove_if_exist(struct userdata *u, int is_sink, uint32_t index_to_remove)
{
    struct pa_pm_list *item_info = NULL;
    struct pa_pm_list *item_info_n = NULL;

    /* Remove if exists */
    PA_LLIST_FOREACH_SAFE(item_info, item_info_n, u->pm_list) {
        if (item_info->is_sink == is_sink && item_info->index == index_to_remove) {
            pa_log_debug_verbose("[PM] Found index (%s:%d) exists on list[%p], Remove it",
                    GET_STR(is_sink), index_to_remove, u->pm_list);

            PA_LLIST_REMOVE(struct pa_pm_list, u->pm_list, item_info);
            pa_xfree (item_info);
        }
    }
}

static void _pm_list_dump(struct userdata *u, pa_strbuf *s)
{
    struct pa_pm_list *list_info = NULL;
    struct pa_pm_list *list_info_n = NULL;

    if (u->pm_list) {
        PA_LLIST_FOREACH_SAFE(list_info, list_info_n, u->pm_list) {
            pa_strbuf_printf(s, "[%s:%d]", GET_STR(list_info->is_sink), list_info->index);
        }
    }
    if (pa_strbuf_isempty(s)) {
        pa_strbuf_puts(s, "empty");
    }
}

static void update_pm_status (struct userdata *u, int is_sink, int index, int is_resume)
{
    int ret = -1;
    pa_strbuf *before, *after;

    pa_mutex_lock(u->pm_mutex);

    before = pa_strbuf_new();
    _pm_list_dump(u, before);

    if (is_resume) {
        _pm_list_add_if_not_exist(u, is_sink, index);

        if (u->pm_list) {
            if (!u->is_pm_locked) {
                if ((ret = pm_display_lock()) == -1)
                    pa_log_warn("pm_lock_state failed [%d]", ret);
                else
                    u->is_pm_locked = TRUE;
            } else {
                pa_log_debug("already locked state, skip lock");
            }
        }
    } else {
        _pm_list_remove_if_exist(u, is_sink, index);

        if (u->pm_list == NULL) {
            if (u->is_pm_locked) {
                if ((ret = pm_display_unlock()) == -1)
                    pa_log_warn("pm_unlock_state failed [%d]", ret);
                else
                    u->is_pm_locked = FALSE;
            } else {
                pa_log_debug("already unlocked state, skip unlock");
            }
        }
    }

    after = pa_strbuf_new();
    _pm_list_dump(u, after);
    pa_log_info("[PM] %s [%s:%d] ret[%d] list[%p] before:%s after:%s",
            (is_resume) ? "resume" : "suspend", GET_STR(is_sink), index, ret, u->pm_list,
            pa_strbuf_tostring_free(before), pa_strbuf_tostring_free(after));

    pa_mutex_unlock(u->pm_mutex);
}
#endif /* USE_PM_LOCK */

#ifdef USE_AUDIO_PM
static struct snd_scenario* _alsa_scenario_open();
static int _alsa_scenario_set(struct snd_scenario* scn, char * str);
static int _alsa_scenario_reload(struct snd_scenario* scn);
static void _alsa_scenario_close(struct snd_scenario* scn);

enum {
	SET_INFO,
	UPDATE_SCN,
};

typedef enum {
	ASCN_GAIN = 0,
	ASCN_PATH,
} ASCN_MODE_T;

typedef enum {
	ASCN_PLAYBACK = 0,
	ASCN_CAPTURE,
} ASCN_TYPE_T;

#define	SCN_RESET "reset"
#define	SCN_RESET_PLAYBACK "reset_playback"
#define	SCN_RESET_CAPTURE "reset_capture"

const char* mode_str[] = {"gain", "path"};
const char* type_str[] = {"playback", "capture"};

#define ASCN_GET_TYPE(value) (value & 0x0F)
#define ASCN_GET_MODE(value) (value >> 4)

#define ASCN_IS_CALL_STATUS(value) (value & 0x100)
#define ASCN_CALL_STATUS(value) (value & 0x00F)

#define VCONF_KEY_SOUND_GAIN_PLAYBACK				"memory/private/sound/path/gain_playback"
#define VCONF_KEY_SOUND_PATH_PLAYBACK				"memory/private/sound/path/path_playback"
#define VCONF_KEY_SOUND_GAIN_CAPTURE				"memory/private/sound/path/gain_capture"
#define VCONF_KEY_SOUND_PATH_CAPTURE				"memory/private/sound/path/path_capture"

#define PATH_LOCK(m)  do { \
	/*pa_log_warn("(*)LOCKING");*/ \
	pa_mutex_lock(m); \
	/*pa_log_warn("(+)LOCKED");*/ \
}while(0)

#define PATH_UNLOCK(m)  do { \
	pa_mutex_unlock(m); \
	/*pa_log_warn("(-)UNLOCKED");*/ \
}while(0)

static void _dump_info (ASCN_MODE_T mode, ASCN_TYPE_T type)
{
    pa_log_debug("--------------------------------------------------");
    char *str = NULL;

    if (type == ASCN_PLAYBACK) {
        if (mode == ASCN_GAIN) {
            str = vconf_get_str(VCONF_KEY_SOUND_GAIN_PLAYBACK);
            if (str)
                pa_log_debug("[APM] VCONF_KEY_SOUND_GAIN_PLAYBACK = [%s]", str);
        } else {
            str = vconf_get_str(VCONF_KEY_SOUND_PATH_PLAYBACK);
            if (str)
                pa_log_debug("[APM] VCONF_KEY_SOUND_PATH_PLAYBACK = [%s]", str);
        }
    } else {
        if (mode == ASCN_GAIN) {
            str = vconf_get_str(VCONF_KEY_SOUND_GAIN_CAPTURE);
            if (str)
                pa_log_debug("[APM] VCONF_KEY_SOUND_GAIN_CAPTURE  = [%s]", str);
        } else {
            str = vconf_get_str(VCONF_KEY_SOUND_PATH_CAPTURE);
            if (str)
                pa_log_debug("[APM] VCONF_KEY_SOUND_PATH_CAPTURE  = [%s]", str);
        }
    }
    if (str)
        free(str);
    pa_log_debug("--------------------------------------------------");
}

static void _dump_all_info ()
{
    _dump_info (ASCN_GAIN, ASCN_PLAYBACK);
    _dump_info (ASCN_PATH, ASCN_PLAYBACK);
    _dump_info (ASCN_GAIN, ASCN_CAPTURE);
    _dump_info (ASCN_PATH, ASCN_CAPTURE);
}

static void _set_ascn_str (struct snd_scenario *scn, char* scn_str)
{
    char* str_tok = NULL;
    char* tmp_str = NULL;

    pa_log_debug("[APM] ***** START scn=[%p], scn_str=[%s] ******", scn, scn_str);

    if(scn == NULL || scn_str == NULL) {
        pa_log_warn("[APM] Invalid argument...return...");
        return;
    }

    tmp_str = strdup (scn_str);
    str_tok = strtok (tmp_str, ";");
    while (str_tok) {
        pa_log_debug("[APM] => Set ASCN [%s]", str_tok);
        ymu831_set_scenario (str_tok);
        _alsa_scenario_set(scn, str_tok);
        str_tok = strtok (NULL, ";");
    }
    free (tmp_str);
    pa_log_debug("[APM] ***** END [%s] ******", scn_str);
}


static void _set_playback (struct snd_scenario *scn, ASCN_MODE_T mode)
{
    char *str = NULL;

    if (scn == NULL) {
        pa_log_error("[APM] scn is null");
        return;
    }

    if (mode == ASCN_GAIN) {
        _alsa_scenario_set(scn, SCN_RESET_PLAYBACK);
        str = vconf_get_str(VCONF_KEY_SOUND_GAIN_PLAYBACK);
    } else {
        str = vconf_get_str(VCONF_KEY_SOUND_PATH_PLAYBACK);
    }

    if (str) {
        _set_ascn_str (scn, str);
        free(str);
    }
}

static void _set_capture (struct snd_scenario *scn, ASCN_MODE_T mode)
{
    char *str = NULL;

    if (scn == NULL) {
        pa_log_error("[APM] scn is null");
        return;
    }

    if (mode == ASCN_GAIN) {
        _alsa_scenario_set(scn, SCN_RESET_CAPTURE);
        str = vconf_get_str(VCONF_KEY_SOUND_GAIN_CAPTURE);
    } else {
        str = vconf_get_str(VCONF_KEY_SOUND_PATH_CAPTURE);
    }

    if (str) {
        _set_ascn_str (scn, str);
        free(str);
    }
}

static int extension_cb(pa_native_protocol *p, pa_module *m, pa_native_connection *c, uint32_t tag, pa_tagstruct *t) {
    uint32_t command;
    uint32_t value;

    ASCN_MODE_T mode;
    ASCN_TYPE_T type;

    struct userdata *u = NULL;
    pa_tagstruct *reply = NULL;
    pa_assert(p);
    pa_assert(m);
    pa_assert(c);
    pa_assert(t);

    u = m->userdata;

    if (pa_tagstruct_getu32(t, &command) < 0)
        goto fail;

    switch (command) {
    case SET_INFO: {
        if (pa_tagstruct_getu32(t, &value) < 0)
            goto fail;

        PATH_LOCK(u->m);

        if (ASCN_IS_CALL_STATUS(value)) {
            pa_log_debug("[APM] ##### SUSPEND_ON_IDLE got = CALL STATUS [%d]#####", ASCN_CALL_STATUS(value));
            u->is_on_call = ASCN_CALL_STATUS(value);
        } else { /* Other than Call status */
            mode = ASCN_GET_MODE(value);
            type = ASCN_GET_TYPE(value);

            pa_log_debug("[APM] ##### SUSPEND_ON_IDLE got = [0x%02x][%s_%s], use_audio_pm=[%d] #####",
                    value, mode_str[mode], type_str[type], u->use_audio_pm);

            _dump_info(mode, type);

            /* Check if need to be set path immediately */
            if (u->use_audio_pm && !u->is_on_call) {
                if (type == ASCN_PLAYBACK && !u->is_sink_suspended) {
                    pa_log_debug("[APM] Sink is ACTIVE => SET NOW");
                    _set_playback(u->scn, mode);
                } else if (type == ASCN_CAPTURE && !u->is_source_suspended) {
                    pa_log_debug("[APM] Source is ACTIVE => SET NOW");
                    _set_capture(u->scn, mode);
                }
            }

            pa_log_debug("[APM] ####################################################################");
        }

        PATH_UNLOCK(u->m);
        break;
    }

    case UPDATE_SCN:
        /* FIXME: if _alsa_scenario_reload() works fine, replace with this
         * close and reopen handle */
        PATH_LOCK(u->m);
        _alsa_scenario_close(u->scn);
        u->scn = _alsa_scenario_open();
        PATH_UNLOCK(u->m);
        break;

    default:
        goto fail;
    }
    reply = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);
    pa_pstream_send_tagstruct(pa_native_connection_get_pstream(c), reply);
    return 0;

fail:
    return -1;
}

/* ALSA scenario stuff */
#define CARD_NUMBER 0 /* ALSA card number */
#define DEFAULT_CARD	"default"

static struct snd_scenario* _alsa_scenario_open()
{
    int card = CARD_NUMBER;
    char *name = NULL;
    struct snd_scenario* scn = NULL;

    /* Try to get card name from CARD_NUMBER. */
    snd_card_get_name(card, &name);
    if (name) { /* Name exists, use that name to open */
        pa_log_debug ("Name exists, use that name to open");
        scn = snd_scenario_open(name);
        if (scn == NULL) { /* Failed to open with given name, try with DEFAULT CARD again */
            pa_log_debug ("Failed to open with given name, try with DEFAULT CARD again");
            scn = snd_scenario_open(DEFAULT_CARD);
        }
        free(name);
    } else { /* No Name exists, use DEFAULT CARD */
        pa_log_debug ("No Name exists, use DEFAULT CARD");
        scn = snd_scenario_open(DEFAULT_CARD);
    }

    pa_log_debug ("return scn=[%p]", scn);
    return scn;
}

static int _alsa_scenario_set(struct snd_scenario* scn, char * str)
{
    int err = 0;

    if (str == NULL)
        return -1;

    /* Set scenario */
    err = snd_scenario_set_scn(scn, str);
    if (err < 0) {
        pa_log_error("snd_scenario_set(%s) failed\n", str);
    } else {
        pa_log_debug ("snd_scenario_set(%s) success!!!!\n", str);
    }
    return err;
}

static int _alsa_scenario_reload(struct snd_scenario* scn)
{
    int err = snd_scenario_reload(scn);
    pa_log_debug("snd_scenario_reload ret = [%d]\n", err);
    return err;
}

static void _alsa_scenario_close(struct snd_scenario* scn)
{
    if (scn)
        snd_scenario_close(scn);
}

#endif /* USE_AUDIO_PM */


static pa_bool_t _is_audio_pm_needed (pa_proplist *p)
{
    char* value = pa_proplist_gets (p, "need_audio_pm");
    if (value) {
        return atoi (value);
    }
    return 0;
}

static void timeout_cb(pa_mainloop_api*a, pa_time_event* e, const struct timeval *t, void *userdata) {
    struct device_info *d = userdata;
    int ret = -1;

    pa_assert(d);

    d->userdata->core->mainloop->time_restart(d->time_event, NULL);

    /* SINK */
    if (d->sink && pa_sink_check_suspend(d->sink) <= 0 && !(d->sink->suspend_cause & PA_SUSPEND_IDLE)) {
        pa_log_info_verbose("Sink %s idle for too long, suspending ...", d->sink->name);
        pa_sink_suspend(d->sink, TRUE, PA_SUSPEND_IDLE);
#ifdef USE_AUDIO_PM
        if (d->userdata->use_audio_pm) {
            PATH_LOCK(d->userdata->m);
            pa_log_debug("[APM] SUSPEND : is_on_call=[%d], audio_pm_needed=[%d], is_sink_suspended=[%d]",
                        d->userdata->is_on_call, _is_audio_pm_needed(d->sink->proplist), d->userdata->is_sink_suspended);
            if (_is_audio_pm_needed(d->sink->proplist)) {
                if (!d->userdata->is_on_call) {
                    pa_log_debug("[APM] Do RESET_PLAYBACK ----- ");
                    _alsa_scenario_set (d->userdata->scn, SCN_RESET_PLAYBACK);
                }
                d->userdata->is_sink_suspended = TRUE;
            } else {
                pa_log_debug("[APM] This sink doesn't need power control");
            }

            PATH_UNLOCK(d->userdata->m);
        }
#endif /* USE_AUDIO_PM */

#ifdef USE_PM_LOCK
        update_pm_status(d->userdata, PM_SINK, d->sink->index, PM_SUSPEND);
#endif /* USE_PM_LOCK */
    }

    /* SOURCE */
    if (d->source && pa_source_check_suspend(d->source) <= 0 && !(d->source->suspend_cause & PA_SUSPEND_IDLE)) {
        pa_log_info("Source %s idle for too long, suspending ...", d->source->name);
        pa_source_suspend(d->source, TRUE, PA_SUSPEND_IDLE);
#ifdef USE_AUDIO_PM
        if (d->userdata->use_audio_pm) {
            PATH_LOCK(d->userdata->m);
            pa_log_debug("[APM] SUSPEND : is_on_call=[%d], audio_pm_needed=[%d], is_source_suspended=[%d]",
                         d->userdata->is_on_call, _is_audio_pm_needed(d->source->proplist), d->userdata->is_source_suspended);
            if (_is_audio_pm_needed(d->source->proplist)) {
                if (!d->userdata->is_on_call) {
                    pa_log_debug("[APM] Do RESET_CAPTURE -----");
                    _alsa_scenario_set (d->userdata->scn, SCN_RESET_CAPTURE);
                }
                d->userdata->is_source_suspended = TRUE;
            } else {
               pa_log_debug("[APM] This source doesn't need power control");
            }
            PATH_UNLOCK(d->userdata->m);
        }
#endif /* USE_AUDIO_PM */

#ifdef USE_PM_LOCK
        update_pm_status(d->userdata, PM_SOURCE, d->source->index, PM_SUSPEND);
#endif /* USE_PM_LOCK */
    }

#ifdef USE_AUDIO_PM
    /* If not in call & sink/source are pm needed & both sink/source are suspended, do complete RESET */
    PATH_LOCK(d->userdata->m);
    if (!d->userdata->is_on_call &&
        ((d->source && _is_audio_pm_needed(d->source->proplist)) ||
        (d->sink && _is_audio_pm_needed(d->sink->proplist)))) {
        if (d->userdata->is_sink_suspended && d->userdata->is_source_suspended) {
            pa_log_debug("[APM] Do RESET");
            _alsa_scenario_set (d->userdata->scn, SCN_RESET);
        }
    }
    PATH_UNLOCK(d->userdata->m);
#endif /* USE_AUDIO_PM */
}

static void restart(struct device_info *d, int input_timeout) {
    pa_usec_t now;
    const char *s = NULL;
    uint32_t timeout = d->userdata->timeout;

    pa_assert(d);
    pa_assert(d->sink || d->source);

    d->last_use = now = pa_rtclock_now();

    s = pa_proplist_gets(d->sink ? d->sink->proplist : d->source->proplist, "module-suspend-on-idle.timeout");
    if (!s || pa_atou(s, &timeout) < 0) {
        if (input_timeout >= 0)
            timeout = (uint32_t)input_timeout;
    }

#ifdef __TIZEN__
    s = pa_proplist_gets(d->sink ? d->sink->proplist : d->source->proplist, PA_PROP_DEVICE_STRING);
    if (s && (pa_streq (s, "hw:0,3") || pa_streq (s,"hw:0,1"))) {
        if (d->sink)
            pa_log_warn("Sink %s becomes idle, this is VOIP/HDMI device, skip restart timer", d->sink->name);
        if (d->source)
            pa_log_warn("Source %s becomes idle, this is VOIP/HDMI device, skip restart timer.", d->source->name);
        return;
    }
#endif

#ifdef __TIZEN__
    /* Assume that timeout is milli seconds unit if large (>=100) enough */
    if (timeout >= 100) {
        pa_core_rttime_restart(d->userdata->core, d->time_event, now + timeout * PA_USEC_PER_MSEC);

        if (d->sink)
            pa_log_debug_verbose("Sink %s becomes idle, timeout in %u msec.", d->sink->name, timeout);
        if (d->source)
            pa_log_debug_verbose("Source %s becomes idle, timeout in %u msec.", d->source->name, timeout);
    } else {
        pa_core_rttime_restart(d->userdata->core, d->time_event, now + timeout * PA_USEC_PER_SEC);

        if (d->sink)
            pa_log_debug_verbose("Sink %s becomes idle, timeout in %u seconds.", d->sink->name, timeout);
        if (d->source)
            pa_log_debug_verbose("Source %s becomes idle, timeout in %u seconds.", d->source->name, timeout);
    }
#else
    pa_core_rttime_restart(d->userdata->core, d->time_event, now + timeout * PA_USEC_PER_SEC);

    if (d->sink)
        pa_log_debug_verbose("Sink %s becomes idle, timeout in %u seconds.", d->sink->name, timeout);
    if (d->source)
        pa_log_debug_verbose("Source %s becomes idle, timeout in %u seconds.", d->source->name, timeout);
#endif
}

static void resume(struct device_info *d) {
    pa_assert(d);

    d->userdata->core->mainloop->time_restart(d->time_event, NULL);

    if (d->sink) {
#ifdef USE_PM_LOCK
        update_pm_status(d->userdata, PM_SINK, d->sink->index, PM_RESUME);
#endif /* USE_PM_LOCK */

#ifdef USE_AUDIO_PM
        if (d->userdata->use_audio_pm) {
            PATH_LOCK(d->userdata->m);
            pa_log_debug("[APM] RESUME : is_on_call=[%d], audio_pm_needed=[%d], is_sink_suspended=[%d]",
                    d->userdata->is_on_call, _is_audio_pm_needed(d->sink->proplist), d->userdata->is_sink_suspended);
            if (_is_audio_pm_needed(d->sink->proplist)) {
                if (!d->userdata->is_on_call && d->userdata->is_sink_suspended) {
                    pa_log_debug("[APM] DO PLAYBACK PATH +++++");
                    _set_playback(d->userdata->scn, ASCN_GAIN);
                    _set_playback(d->userdata->scn, ASCN_PATH);
                }
                d->userdata->is_sink_suspended = FALSE;
            } else {
                pa_log_debug("[APM] This sink doesn't need power control");
            }
            PATH_UNLOCK(d->userdata->m);
        }
#endif /* USE_AUDIO_PM */

        pa_sink_suspend(d->sink, FALSE, PA_SUSPEND_IDLE);
        pa_log_debug_verbose("Sink %s becomes busy.", d->sink->name);
    }

    if (d->source) {
#ifdef USE_PM_LOCK
        update_pm_status(d->userdata, PM_SOURCE, d->source->index, PM_RESUME);
#endif /* USE_PM_LOCK */

#ifdef USE_AUDIO_PM
        if (d->userdata->use_audio_pm) {
            PATH_LOCK(d->userdata->m);
            pa_log_debug("[APM] RESUME : is_on_call=[%d], audio_pm_needed=[%d], is_source_suspended=[%d]",
                    d->userdata->is_on_call, _is_audio_pm_needed(d->source->proplist), d->userdata->is_source_suspended);
            if (_is_audio_pm_needed(d->source->proplist)) {
                if (!d->userdata->is_on_call && d->userdata->is_source_suspended) {
                    pa_log_debug("[APM] DO CAPTURE PATH +++++");
                    _set_capture(d->userdata->scn, ASCN_GAIN);
                    _set_capture(d->userdata->scn, ASCN_PATH);
                }
                d->userdata->is_source_suspended = FALSE;
            } else {
                pa_log_debug("[APM] This source doesn't need power control");
            }
            PATH_UNLOCK(d->userdata->m);
        }
#endif /* USE_AUDIO_PM */

        pa_source_suspend(d->source, FALSE, PA_SUSPEND_IDLE);
        pa_log_debug_verbose("Source %s becomes busy.", d->source->name);
    }
}

static pa_hook_result_t sink_input_fixate_hook_cb(pa_core *c, pa_sink_input_new_data *data, struct userdata *u) {
    struct device_info *d;

    pa_assert(c);
    pa_assert(data);
    pa_assert(u);

    /* We need to resume the audio device here even for
     * PA_SINK_INPUT_START_CORKED, since we need the device parameters
     * to be fully available while the stream is set up. */

    if ((d = pa_hashmap_get(u->device_infos, data->sink)))
        resume(d);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_fixate_hook_cb(pa_core *c, pa_source_output_new_data *data, struct userdata *u) {
    struct device_info *d;

    pa_assert(c);
    pa_assert(data);
    pa_assert(u);

    if (data->source->monitor_of)
        d = pa_hashmap_get(u->device_infos, data->source->monitor_of);
    else
        d = pa_hashmap_get(u->device_infos, data->source);

    if (d)
        resume(d);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_unlink_hook_cb(pa_core *c, pa_sink_input *s, struct userdata *u) {
    pa_assert(c);
    pa_sink_input_assert_ref(s);
    pa_assert(u);

    if (!s->sink)
        return PA_HOOK_OK;

    if (pa_sink_check_suspend(s->sink) <= 0) {
        struct device_info *d;
        if ((d = pa_hashmap_get(u->device_infos, s->sink)))
            restart(d, USE_DEVICE_DEFAULT_TIMEOUT);
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_unlink_hook_cb(pa_core *c, pa_source_output *s, struct userdata *u) {
    struct device_info *d = NULL;
    int n_source_output = 0;
    int timeout = USE_DEVICE_DEFAULT_TIMEOUT;
    const int nothing = 0;

    pa_assert(c);
    pa_source_output_assert_ref(s);
    pa_assert(u);

    if (!s->source)
        return PA_HOOK_OK;

    if (s->source->monitor_of) {
        if (pa_sink_check_suspend(s->source->monitor_of) <= 0)
            d = pa_hashmap_get(u->device_infos, s->source->monitor_of);
    } else {
        if (pa_source_check_suspend(s->source) <= 0)
            d = pa_hashmap_get(u->device_infos, s->source);
    }

    n_source_output = pa_source_linked_by(s->source);
    if(n_source_output == nothing) {
        timeout = 0; // set timeout 0, should be called immediately.
        pa_log_error("source outputs does't exist anymore. enter suspend state. name(%s), count(%d)",
            s->source->name, n_source_output);
    }

    if (d)
        restart(d, timeout);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_move_start_hook_cb(pa_core *c, pa_sink_input *s, struct userdata *u) {
    struct device_info *d;

    pa_assert(c);
    pa_sink_input_assert_ref(s);
    pa_assert(u);

    if (pa_sink_check_suspend(s->sink) <= 1)
        if ((d = pa_hashmap_get(u->device_infos, s->sink)))
            restart(d, USE_DEVICE_DEFAULT_TIMEOUT);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_move_finish_hook_cb(pa_core *c, pa_sink_input *s, struct userdata *u) {
    struct device_info *d;
    pa_sink_input_state_t state;

    pa_assert(c);
    pa_sink_input_assert_ref(s);
    pa_assert(u);

    state = pa_sink_input_get_state(s);
    if (state != PA_SINK_INPUT_RUNNING && state != PA_SINK_INPUT_DRAINED)
        return PA_HOOK_OK;

    if ((d = pa_hashmap_get(u->device_infos, s->sink)))
        resume(d);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_move_start_hook_cb(pa_core *c, pa_source_output *s, struct userdata *u) {
    struct device_info *d = NULL;

    pa_assert(c);
    pa_source_output_assert_ref(s);
    pa_assert(u);

    if (s->source->monitor_of) {
        if (pa_sink_check_suspend(s->source->monitor_of) <= 1)
            d = pa_hashmap_get(u->device_infos, s->source->monitor_of);
    } else {
        if (pa_source_check_suspend(s->source) <= 1)
            d = pa_hashmap_get(u->device_infos, s->source);
    }

    if (d)
        restart(d, USE_DEVICE_DEFAULT_TIMEOUT);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_move_finish_hook_cb(pa_core *c, pa_source_output *s, struct userdata *u) {
    struct device_info *d;

    pa_assert(c);
    pa_source_output_assert_ref(s);
    pa_assert(u);

    if (pa_source_output_get_state(s) != PA_SOURCE_OUTPUT_RUNNING)
        return PA_HOOK_OK;

    if (s->source->monitor_of)
        d = pa_hashmap_get(u->device_infos, s->source->monitor_of);
    else
        d = pa_hashmap_get(u->device_infos, s->source);

    if (d)
        resume(d);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_state_changed_hook_cb(pa_core *c, pa_sink_input *s, struct userdata *u) {
    struct device_info *d;
    pa_sink_input_state_t state;

    pa_assert(c);
    pa_sink_input_assert_ref(s);
    pa_assert(u);

    state = pa_sink_input_get_state(s);
    if (state == PA_SINK_INPUT_RUNNING || state == PA_SINK_INPUT_DRAINED)
        if ((d = pa_hashmap_get(u->device_infos, s->sink)))
            resume(d);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_state_changed_hook_cb(pa_core *c, pa_source_output *s, struct userdata *u) {
    pa_assert(c);
    pa_source_output_assert_ref(s);
    pa_assert(u);

    if (pa_source_output_get_state(s) == PA_SOURCE_OUTPUT_RUNNING) {
        struct device_info *d;

        if (s->source->monitor_of)
            d = pa_hashmap_get(u->device_infos, s->source->monitor_of);
        else
            d = pa_hashmap_get(u->device_infos, s->source);

        if (d)
            resume(d);
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t device_new_hook_cb(pa_core *c, pa_object *o, struct userdata *u) {
    struct device_info *d;
    pa_source *source;
    pa_sink *sink;

    pa_assert(c);
    pa_object_assert_ref(o);
    pa_assert(u);

    source = pa_source_isinstance(o) ? PA_SOURCE(o) : NULL;
    sink = pa_sink_isinstance(o) ? PA_SINK(o) : NULL;

    /* Never suspend monitors */
    if (source && source->monitor_of)
        return PA_HOOK_OK;

    pa_assert(source || sink);

    d = pa_xnew(struct device_info, 1);
    d->userdata = u;
    d->source = source ? pa_source_ref(source) : NULL;
    d->sink = sink ? pa_sink_ref(sink) : NULL;
    d->time_event = pa_core_rttime_new(c, PA_USEC_INVALID, timeout_cb, d);
    pa_hashmap_put(u->device_infos, o, d);

    if ((d->sink && pa_sink_check_suspend(d->sink) <= 0) ||
        (d->source && pa_source_check_suspend(d->source) <= 0))
        restart(d, USE_DEVICE_DEFAULT_TIMEOUT);

    return PA_HOOK_OK;
}

static void device_info_free(struct device_info *d) {
    pa_assert(d);

    if (d->source)
        pa_source_unref(d->source);
    if (d->sink)
        pa_sink_unref(d->sink);

    d->userdata->core->mainloop->time_free(d->time_event);

    pa_xfree(d);
}

static pa_hook_result_t device_unlink_hook_cb(pa_core *c, pa_object *o, struct userdata *u) {
    struct device_info *d;
    pa_sink *sink = NULL;
    pa_source *source = NULL;

    pa_assert(c);
    pa_object_assert_ref(o);
    pa_assert(u);

    if (pa_sink_isinstance(o)) {
        sink = PA_SINK(o);
        pa_log_info ("sink [%p][%d] is unlinked, now update pm", sink, (sink)? sink->index : -1);
        update_pm_status(u, PM_SINK, sink->index, PM_SUSPEND);
    } else if (pa_source_isinstance(o)) {
        source = PA_SOURCE(o);
        pa_log_info ("source [%p][%d] is unlinked, now update pm", source, (source)? source->index : -1);
        update_pm_status(u, PM_SOURCE, source->index, PM_SUSPEND);
    }

    if ((d = pa_hashmap_remove(u->device_infos, o)))
        device_info_free(d);

    return PA_HOOK_OK;
}

static pa_hook_result_t device_state_changed_hook_cb(pa_core *c, pa_object *o, struct userdata *u) {
    struct device_info *d;

    pa_assert(c);
    pa_object_assert_ref(o);
    pa_assert(u);

    if (!(d = pa_hashmap_get(u->device_infos, o)))
        return PA_HOOK_OK;

    if (pa_sink_isinstance(o)) {
        pa_sink *s = PA_SINK(o);
        pa_sink_state_t state = pa_sink_get_state(s);

#ifdef USE_PM_LOCK
        if(state == PA_SINK_SUSPENDED && (s->suspend_cause & PA_SUSPEND_USER))
            update_pm_status(d->userdata, PM_SINK, d->sink->index, PM_SUSPEND);
#endif
        if (pa_sink_check_suspend(s) <= 0)
            if (PA_SINK_IS_OPENED(state))
                restart(d, USE_DEVICE_DEFAULT_TIMEOUT);

    } else if (pa_source_isinstance(o)) {
        pa_source *s = PA_SOURCE(o);
        pa_source_state_t state = pa_source_get_state(s);

        if (pa_source_check_suspend(s) <= 0)
            if (PA_SOURCE_IS_OPENED(state))
                restart(d, USE_DEVICE_DEFAULT_TIMEOUT);
    }

    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    uint32_t timeout = 5;
#ifdef USE_AUDIO_PM
    pa_bool_t use_audio_pm = FALSE;
#endif /* USE_AUDIO_PM */
    uint32_t idx;
    pa_sink *sink;
    pa_source *source;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    if (pa_modargs_get_value_u32(ma, "timeout", &timeout) < 0) {
        pa_log("Failed to parse timeout value.");
        goto fail;
    }

#ifdef USE_AUDIO_PM
    if (pa_modargs_get_value_boolean(ma, "use_audio_pm", &use_audio_pm) < 0) {
		pa_log("Failed to parse use_audio_pm value.");
		goto fail;
	}
#endif /* USE_AUDIO_PM */

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->core = m->core;
    u->timeout = timeout;
    u->device_infos = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
#ifdef USE_PM_LOCK
    PA_LLIST_HEAD_INIT(pa_pm_list, u->pm_list);
#endif /* USE_PM_LOCK */
#ifdef USE_AUDIO_PM
    u->use_audio_pm = use_audio_pm;
    u->is_on_call = 0;
#endif /* USE_AUDIO_PM */

    for (sink = pa_idxset_first(m->core->sinks, &idx); sink; sink = pa_idxset_next(m->core->sinks, &idx))
        device_new_hook_cb(m->core, PA_OBJECT(sink), u);

    for (source = pa_idxset_first(m->core->sources, &idx); source; source = pa_idxset_next(m->core->sources, &idx))
        device_new_hook_cb(m->core, PA_OBJECT(source), u);

    u->sink_new_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_PUT], PA_HOOK_NORMAL, (pa_hook_cb_t) device_new_hook_cb, u);
    u->source_new_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_PUT], PA_HOOK_NORMAL, (pa_hook_cb_t) device_new_hook_cb, u);
    u->sink_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_UNLINK_POST], PA_HOOK_NORMAL, (pa_hook_cb_t) device_unlink_hook_cb, u);
    u->source_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_UNLINK_POST], PA_HOOK_NORMAL, (pa_hook_cb_t) device_unlink_hook_cb, u);
    u->sink_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_STATE_CHANGED], PA_HOOK_NORMAL, (pa_hook_cb_t) device_state_changed_hook_cb, u);
    u->source_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_STATE_CHANGED], PA_HOOK_NORMAL, (pa_hook_cb_t) device_state_changed_hook_cb, u);

    u->sink_input_new_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_FIXATE], PA_HOOK_NORMAL, (pa_hook_cb_t) sink_input_fixate_hook_cb, u);
    u->source_output_new_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_FIXATE], PA_HOOK_NORMAL, (pa_hook_cb_t) source_output_fixate_hook_cb, u);
    u->sink_input_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK_POST], PA_HOOK_NORMAL, (pa_hook_cb_t) sink_input_unlink_hook_cb, u);
    u->source_output_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_UNLINK_POST], PA_HOOK_NORMAL, (pa_hook_cb_t) source_output_unlink_hook_cb, u);
    u->sink_input_move_start_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_START], PA_HOOK_NORMAL, (pa_hook_cb_t) sink_input_move_start_hook_cb, u);
    u->source_output_move_start_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_START], PA_HOOK_NORMAL, (pa_hook_cb_t) source_output_move_start_hook_cb, u);
    u->sink_input_move_finish_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FINISH], PA_HOOK_NORMAL, (pa_hook_cb_t) sink_input_move_finish_hook_cb, u);
    u->source_output_move_finish_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_FINISH], PA_HOOK_NORMAL, (pa_hook_cb_t) source_output_move_finish_hook_cb, u);
    u->sink_input_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_STATE_CHANGED], PA_HOOK_NORMAL, (pa_hook_cb_t) sink_input_state_changed_hook_cb, u);
    u->source_output_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_STATE_CHANGED], PA_HOOK_NORMAL, (pa_hook_cb_t) source_output_state_changed_hook_cb, u);
#ifdef USE_AUDIO_PM
    u->protocol = pa_native_protocol_get(m->core);
      pa_native_protocol_install_ext(u->protocol, m, extension_cb);

    if(u->use_audio_pm) {
        u->scn = _alsa_scenario_open();
        u->is_sink_suspended = TRUE;
        u->is_source_suspended = TRUE;
    }
    u->m = pa_mutex_new(FALSE, FALSE);
#endif /* USE_AUDIO_PM */

#ifdef USE_PM_LOCK
    u->pm_mutex = pa_mutex_new(FALSE, FALSE);
    u->is_pm_locked = FALSE;
#endif

    pa_modargs_free(ma);
    return 0;

fail:

    if (ma)
        pa_modargs_free(ma);

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;
    struct device_info *d;

    pa_assert(m);

    if (!m->userdata)
        return;

    u = m->userdata;

    if (u->sink_new_slot)
        pa_hook_slot_free(u->sink_new_slot);
    if (u->sink_unlink_slot)
        pa_hook_slot_free(u->sink_unlink_slot);
    if (u->sink_state_changed_slot)
        pa_hook_slot_free(u->sink_state_changed_slot);

    if (u->source_new_slot)
        pa_hook_slot_free(u->source_new_slot);
    if (u->source_unlink_slot)
        pa_hook_slot_free(u->source_unlink_slot);
    if (u->source_state_changed_slot)
        pa_hook_slot_free(u->source_state_changed_slot);

    if (u->sink_input_new_slot)
        pa_hook_slot_free(u->sink_input_new_slot);
    if (u->sink_input_unlink_slot)
        pa_hook_slot_free(u->sink_input_unlink_slot);
    if (u->sink_input_move_start_slot)
        pa_hook_slot_free(u->sink_input_move_start_slot);
    if (u->sink_input_move_finish_slot)
        pa_hook_slot_free(u->sink_input_move_finish_slot);
    if (u->sink_input_state_changed_slot)
        pa_hook_slot_free(u->sink_input_state_changed_slot);

    if (u->source_output_new_slot)
        pa_hook_slot_free(u->source_output_new_slot);
    if (u->source_output_unlink_slot)
        pa_hook_slot_free(u->source_output_unlink_slot);
    if (u->source_output_move_start_slot)
        pa_hook_slot_free(u->source_output_move_start_slot);
    if (u->source_output_move_finish_slot)
        pa_hook_slot_free(u->source_output_move_finish_slot);
    if (u->source_output_state_changed_slot)
        pa_hook_slot_free(u->source_output_state_changed_slot);

    while ((d = pa_hashmap_steal_first(u->device_infos)))
        device_info_free(d);

    pa_hashmap_free(u->device_infos, NULL);

#ifdef USE_AUDIO_PM
    if (u->protocol) {
        pa_native_protocol_remove_ext(u->protocol, m);
        pa_native_protocol_unref(u->protocol);
    }
    if(u->use_audio_pm && u->scn) {
       _alsa_scenario_close(u->scn);
       u->scn = NULL;
    }
    if (u->m) {
        pa_mutex_free(u->m);
        u->m = NULL;
    }
#endif /* USE_AUDIO_PM */

#ifdef USE_PM_LOCK
    if (u->pm_mutex) {
        pa_mutex_free(u->pm_mutex);
        u->pm_mutex = NULL;
    }
#endif
    pa_xfree(u);
}
