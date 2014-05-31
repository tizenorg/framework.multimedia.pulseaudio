/***
  This file is part of PulseAudio.

  Copyright 2013 Seungbae Shin <seungbae.shin@samsung.com>

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

#include <stdbool.h>
#include <strings.h>
#include <vconf.h> // for mono
#include <iniparser.h>
#include <dlfcn.h>
#include <asoundlib.h>

#include <pulse/proplist.h>
#include <pulse/timeval.h>
#include <pulse/util.h>
#include <pulse/rtclock.h>

#include <pulsecore/core.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/core-scache.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/core-util.h>
#include <pulsecore/mutex.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/protocol-native.h>
#include <pulsecore/pstream-util.h>
#include <pulsecore/strbuf.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/sound-file.h>
#include <pulsecore/play-memblockq.h>

#include "module-policy-symdef.h"
#include "tizen-audio.h"

#define VCONFKEY_SOUND_CAPTURE_STATUS "memory/Sound/SoundCaptureStatus" // Should be removed.
#define VCONFKEY_SOUND_HDMI_SUPPORT "memory/private/sound/hdmisupport"
PA_MODULE_AUTHOR("Seungbae Shin");
PA_MODULE_DESCRIPTION("Media Policy module");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
PA_MODULE_USAGE(
        "on_hotplug=<When new device becomes available, recheck streams?> "
        "use_wideband_voice=<Set to 1 to enable wb voice. Default nb>"
        "fragment_size=<fragment size>"
        "tsched_buffer_size=<buffer size when using timer based scheduling> ");

static const char* const valid_modargs[] = {
    "on_hotplug",
    "use_wideband_voice",
    "fragment_size",
    "tsched_buffersize",
    "tsched_buffer_size",
    NULL
};

struct pa_alsa_sink_info {
    const char *pcm_device;
    pa_module *sink;
    pa_module *mono_sink;
    struct pa_alsa_sink_info *prev, *next;
    char *name;
};


struct pa_alsa_source_info {
    const char *pcm_device;
    pa_module *source;
    struct pa_alsa_source_info *prev, *next;
    char *name;
};


struct userdata {
    pa_core *core;
    pa_module *module;

    pa_hook_slot *sink_input_new_hook_slot,*sink_put_hook_slot;

    pa_hook_slot *sink_input_unlink_slot,*sink_unlink_slot;
    pa_hook_slot *sink_input_unlink_post_slot, *sink_unlink_post_slot;
    pa_hook_slot *sink_input_move_start_slot,*sink_input_move_finish_slot;

    pa_subscription *subscription;

    pa_bool_t on_hotplug:1;
    int bt_off_idx;

    uint32_t session;
    uint32_t subsession;
    uint32_t subsession_opt;
    uint32_t active_device_in;
    uint32_t active_device_out;
    uint32_t active_route_flag;

    int is_mono;
    float balance;
    int muteall;
    int call_muted;

    void *handle;
    void *uc_mgr;
    char *playback_pcm;
    char *capture_pcm;
    snd_pcm_t *snd_playback_pcm;
    snd_pcm_t *snd_capture_pcm;
    pa_bool_t wideband;
    int fragment_size;
    int tsched_buffer_size;

    pa_module* module_mono_bt;
    pa_module* module_combined;
    pa_module* module_mono_combined;
    pa_native_protocol *protocol;
    pa_hook_slot *source_output_new_hook_slot;

    PA_LLIST_HEAD(struct pa_alsa_sink_info, alsa_sinks);
    PA_LLIST_HEAD(struct pa_alsa_source_info, alsa_sources);

    struct {
        void *dl_handle;
        void *data;
        audio_return_t (*init)(void **userdata);
        audio_return_t (*deinit)(void **userdata);
        audio_return_t (*reset)(void **userdata);
        audio_return_t (*get_volume_level_max)(void *userdata, uint32_t volume_type, uint32_t *level);
        audio_return_t (*get_volume_level)(void *userdata, uint32_t volume_type, uint32_t *level);
        audio_return_t (*get_volume_value)(void *userdata, audio_info_t *info, uint32_t volume_type, uint32_t level, double *value);
        audio_return_t (*set_volume_level)(void *userdata, audio_info_t *info, uint32_t volume_type, uint32_t level);
        audio_return_t (*get_mute)(void *userdata, audio_info_t *info, uint32_t volume_type, uint32_t direction, uint32_t *mute);
        audio_return_t (*set_mute)(void *userdata, audio_info_t *info, uint32_t volume_type, uint32_t direction, uint32_t mute);
        audio_return_t (*set_session)(void *userdata, uint32_t session, uint32_t subsession);
        audio_return_t (*set_route)(void *userdata, uint32_t session, uint32_t subsession, uint32_t device_in, uint32_t device_out, uint32_t route_flag);
        audio_return_t (*set_ecrx_device)(void *userdata);

        // vsp
        audio_return_t (*set_vsp)(void *userdata, audio_info_t *info, uint32_t value);

        // soundalive
        audio_return_t (*set_soundalive_filter_action)(void* userdata, audio_info_t *info, uint32_t value);
        audio_return_t (*set_soundalive_preset_mode)(void* userdata, audio_info_t *info, uint32_t value);
        audio_return_t (*set_soundalive_equalizer)(void* userdata, audio_info_t *info, uint32_t *eq);
        audio_return_t (*set_soundalive_extend)(void* userdata, audio_info_t *info, uint32_t *ext);
        audio_return_t (*set_soundalive_device)(void* userdata, audio_info_t *info, uint32_t value);
        audio_return_t (*set_dha_param)(void* userdata, audio_info_t *info, uint32_t onoff, uint32_t *gain);
    } audio_mgr;

    struct  { // for burst-shot
        pa_bool_t is_running;
        pa_mutex* mutex;
        int count; /* loop count */
        pa_time_event *time_event;
        pa_scache_entry *e;
        pa_sink_input *i;
        pa_memblockq *q;
        pa_usec_t time_interval;
        pa_usec_t factor; /* timer boosting */
    } audio_sample_userdata;
};

// for soundalive
#define CUSTOM_EQ_BAND_MAX      9
#define EQ_USER_SLOT_NUM        7
#define DHA_GAIN_NUM            12

enum {
    CUSTOM_EXT_3D_LEVEL,
    CUSTOM_EXT_BASS_LEVEL,
    CUSTOM_EXT_CONCERT_HALL_VOLUME,
    CUSTOM_EXT_CONCERT_HALL_LEVEL,
    CUSTOM_EXT_CLARITY_LEVEL,
    CUSTOM_EXT_PARAM_MAX
};

enum {
    SUBCOMMAND_TEST,
    SUBCOMMAND_PLAY_SAMPLE,
    SUBCOMMAND_PLAY_SAMPLE_CONTINUOUSLY,
    SUBCOMMAND_MONO,
    SUBCOMMAND_BALANCE,
    SUBCOMMAND_MUTEALL,
    SUBCOMMAND_SET_USE_CASE,
    SUBCOMMAND_SET_SESSION,
    SUBCOMMAND_SET_SUBSESSION,
    SUBCOMMAND_SET_ACTIVE_DEVICE,
    SUBCOMMAND_RESET,
    SUBCOMMAND_GET_VOLUME_LEVEL_MAX,
    SUBCOMMAND_GET_VOLUME_LEVEL,
    SUBCOMMAND_SET_VOLUME_LEVEL,
    SUBCOMMAND_UPDATE_VOLUME,
    SUBCOMMAND_GET_MUTE,
    SUBCOMMAND_SET_MUTE,
    SUBCOMMAND_IS_AVAILABLE_HIGH_LATENCY,
    SUBCOMMAND_UNLOAD_HDMI,

    // audio filters
    SUBCOMMAND_VSP_SPEED,
    SUBCOMMAND_SA_FILTER_ACTION,
    SUBCOMMAND_SA_PRESET_MODE,
    SUBCOMMAND_SA_EQ,
    SUBCOMMAND_SA_EXTEND,
    SUBCOMMAND_SA_DEVICE,
    SUBCOMMAND_DHA_PARAM,
};

enum {
    SUBSESSION_OPT_SVOICE                   = 0x00000001,
    SUBSESSION_OPT_WAKEUP                   = 0x00000010,
    SUBSESSION_OPT_COMMAND                  = 0x00000020,
};


/* DEFINEs */
#define AEC_SINK            "alsa_output.0.analog-stereo.echo-cancel"
#define AEC_SOURCE          "alsa_input.0.analog-stereo.echo-cancel"
#define SINK_VOIP           "alsa_output.3.analog-stereo"
#define SOURCE_ALSA         "alsa_input.0.analog-stereo"
#define SOURCE_VOIP         "alsa_input.3.analog-stereo"
#define SINK_ALSA           "alsa_output.0.analog-stereo"
#define SINK_MONO_ALSA      "mono_alsa"
#define SINK_MONO_BT        "mono_bt"
#define SINK_MONO_HDMI      "mono_hdmi"
#define SINK_MONO_HIGH_LATENCY    "mono_highlatency"
#define SINK_COMBINED       "combined"
#define SINK_MONO_COMBINED  "mono_combined"
#define SINK_HIGH_LATENCY   "alsa_output.4.analog-stereo"
#define SINK_HDMI           "alsa_output.1.analog-stereo"
#define SOURCE_MIRRORING    "alsa_input.8.analog-stereo"
#define POLICY_AUTO         "auto"
#define POLICY_PHONE        "phone"
#define POLICY_ALL          "all"
#define POLICY_VOIP         "voip"
#define POLICY_HIGH_LATENCY "high-latency"
#define BLUEZ_API           "bluez"
#define ALSA_API            "alsa"
#define VOIP_API            "voip"
#define POLICY_MIRRORING    "mirroring"
#define ALSA_MONITOR_SOURCE "alsa_output.0.analog-stereo.monitor"
#define HIGH_LATENCY_API    "high-latency"
#define NULL_SOURCE         "source.null"

#ifndef PA_DISABLE_MONO_AUDIO
#define MONO_KEY            VCONFKEY_SETAPPL_ACCESSIBILITY_MONO_AUDIO
#endif

#define sink_is_hdmi(sink) !strncmp(sink->name, SINK_HDMI, strlen(SINK_HDMI))
#define sink_is_highlatency(sink) !strncmp(sink->name, SINK_HIGH_LATENCY, strlen(SINK_HIGH_LATENCY))
#define sink_is_mono_highlatency(sink) !strncmp(sink->name, SINK_MONO_HIGH_LATENCY, strlen(SINK_MONO_HIGH_LATENCY))
#define sink_is_alsa(sink) !strncmp(sink->name, SINK_ALSA, strlen(SINK_ALSA))
#define sink_is_voip(sink) !strncmp(sink->name, SINK_VOIP, strlen(SINK_VOIP))

/* Function Declarations */

#define MAX_DEVICES 10
#define MAX_MODIFIER 10
#define DEFAULT_CARD_INDEX 0

#define LIB_UC_MGR "libalsa_intf.so"

#define PCM_DEV_MIC '0'
#define PCM_DEV_VOIP '3'
#define PCM_DEV_LPA '4'
#define PCM_DEV_HDMI '1'

#define CH_5_1 6
#define CH_7_1 8
#define CH_STEREO 2

void* (*uc_mgr_init_p)(char *card) = 0;
void (*uc_mgr_deinit_p)(void *uc_mgr) = 0;
int (*uc_mgr_set_use_case_p)(void *uc_mgr, const char *verb, const char *devices[], const char *modifiers[]) = 0;
int (*uc_mgr_get_use_case_p)(void *uc_mgr, const char *identifier, const char **value) = 0;
int (*uc_mgr_set_modifier_p)(void *uc_mgr, int enable, const char *modifiers[]) = 0;



#define LIB_TIZEN_AUDIO "libtizen-audio.so"
#define PA_DUMP_INI_DEFAULT_PATH                "/usr/etc/mmfw_audio_pcm_dump.ini"
#define PA_DUMP_INI_TEMP_PATH                   "/opt/system/mmfw_audio_pcm_dump.ini"
#define PA_DUMP_VCONF_KEY                       "memory/private/sound/pcm_dump"
#define PA_DUMP_PLAYBACK_DECODER_OUT            0x00000001
#define PA_DUMP_PLAYBACK_SA_SB_IN               0x00000002
#define PA_DUMP_PLAYBACK_SA_SB_OUT              0x00000004
#define PA_DUMP_PLAYBACK_RESAMPLER_IN           0x00000008
#define PA_DUMP_PLAYBACK_RESAMPLER_OUT          0x00000010
#define PA_DUMP_PLAYBACK_DHA_IN                 0x00000020
#define PA_DUMP_PLAYBACK_DHA_OUT                0x00000040
#define PA_DUMP_PLAYBACK_GST_AVSYSTEM_SINK      0x00000400
#define PA_DUMP_PLAYBACK_AVSYSTEM_WRITE         0x00000800
#define PA_DUMP_CAPTURE_AVSYSTEM_READ           0x00100000
#define PA_DUMP_CAPTURE_GST_AVSYSTEM_SRC        0x00200000
#define PA_DUMP_CAPTURE_SEC_RECORD_IN           0x20000000
#define PA_DUMP_CAPTURE_SEC_RECORD_OUT          0x40000000
#define PA_DUMP_CAPTURE_ENCODER_IN              0x80000000

#define MAX_VOLUME_FOR_MONO         65535
/* check if this sink is bluez */

#define DEFAULT_BOOTING_SOUND_PATH "/usr/share/keysound/poweron.wav"
#define BOOTING_SOUND_SAMPLE "booting"
#define VCONF_BOOTING "memory/private/sound/booting"

typedef enum
{
	DOCK_NONE      = 0,
	DOCK_DESKDOCK  = 1,
	DOCK_CARDOCK   = 2,
	DOCK_AUDIODOCK = 7,
	DOCK_SMARTDOCK = 8
} DOCK_STATUS;

static pa_sink *__get_real_master_sink(pa_sink_input *si);
static audio_return_t __fill_audio_stream_info(pa_proplist *sink_input_proplist, pa_sample_spec *sample_spec, audio_info_t *audio_info);
static audio_return_t __fill_audio_device_info(pa_proplist *sink_proplist, audio_info_t *audio_info);
static audio_return_t __fill_audio_info(pa_sink_input *si, audio_info_t *audio_info);
static audio_return_t policy_play_sample(struct userdata *u, pa_native_connection *c, const char *name, uint32_t volume_type, uint32_t gain_type, uint32_t volume_level, uint32_t *stream_idx);
static audio_return_t policy_reset(struct userdata *u);
static audio_return_t policy_set_session(struct userdata *u, uint32_t session, uint32_t start);
static audio_return_t policy_set_active_device(struct userdata *u, uint32_t device_in, uint32_t device_out);
static audio_return_t policy_get_volume_level_max(struct userdata *u, uint32_t volume_type, uint32_t *volume_level);
static audio_return_t __update_volume(struct userdata *u, uint32_t stream_idx, uint32_t volume_type, uint32_t volume_level);
static audio_return_t policy_get_volume_level(struct userdata *u, uint32_t stream_idx, uint32_t *volume_type, uint32_t *volume_level);
static audio_return_t policy_set_volume_level(struct userdata *u, uint32_t stream_idx, uint32_t volume_type, uint32_t volume_level);
static audio_return_t policy_get_mute(struct userdata *u, uint32_t stream_idx, uint32_t volume_type, uint32_t direction, uint32_t *mute);
static audio_return_t policy_set_mute(struct userdata *u, uint32_t stream_idx, uint32_t volume_type, uint32_t direction, uint32_t mute);
static void voip_modules_unloading(struct userdata *u);
static const char *g_volume_vconf[AUDIO_VOLUME_TYPE_MAX] = {
    "file/private/sound/volume/system",         /* AUDIO_VOLUME_TYPE_SYSTEM */
    "file/private/sound/volume/notification",   /* AUDIO_VOLUME_TYPE_NOTIFICATION */
    "file/private/sound/volume/alarm",          /* AUDIO_VOLUME_TYPE_ALARM */
    "file/private/sound/volume/ringtone",       /* AUDIO_VOLUME_TYPE_RINGTONE */
    "file/private/sound/volume/media",          /* AUDIO_VOLUME_TYPE_MEDIA */
    "file/private/sound/volume/call",           /* AUDIO_VOLUME_TYPE_CALL */
    "file/private/sound/volume/voip",           /* AUDIO_VOLUME_TYPE_VOIP */
    "file/private/sound/volume/svoice",         /* AUDIO_VOLUME_TYPE_SVOICE */
    "file/private/sound/volume/fixed",          /* AUDIO_VOLUME_TYPE_FIXED */
    "file/private/sound/volume/java"            /* AUDIO_VOLUME_TYPE_EXT_JAVA */
};

static const char *__get_session_str(uint32_t session)
{
    switch (session) {
    case AUDIO_SESSION_MEDIA:               return "media";
    case AUDIO_SESSION_VOICECALL:           return "voicecall";
    case AUDIO_SESSION_VIDEOCALL:           return "videocall";
    case AUDIO_SESSION_VOIP:                return "voip";
    case AUDIO_SESSION_FMRADIO:             return "fmradio";
    case AUDIO_SESSION_CAMCORDER:           return "camcorder";
    case AUDIO_SESSION_NOTIFICATION:        return "notification";
    case AUDIO_SESSION_EMERGENCY:           return "emergency";
    case AUDIO_SESSION_VOICE_RECOGNITION:   return "vr";
    default:                                return "invalid";
    }
}

static const char *__get_subsession_str(uint32_t subsession)
{
    switch (subsession) {
    case AUDIO_SUBSESSION_NONE:             return "none";
    case AUDIO_SUBSESSION_VOICE:            return "voice";
    case AUDIO_SUBSESSION_RINGTONE:         return "ringtone";
    case AUDIO_SUBSESSION_MEDIA:            return "media";
    case AUDIO_SUBSESSION_VR_INIT:          return "vr_init";
    case AUDIO_SUBSESSION_VR_NORMAL:        return "vr_normal";
    case AUDIO_SUBSESSION_VR_DRIVE:         return "vr_drive";
    case AUDIO_SUBSESSION_STEREO_REC:       return "stereo_rec";
    case AUDIO_SUBSESSION_AM_PLAY:          return "am_play";
    case AUDIO_SUBSESSION_AM_REC:           return "am_rec";
    default:                                return "invalid";
    }
}

static const char *__get_device_in_str(uint32_t device_in)
{
    switch (device_in) {
    case AUDIO_DEVICE_IN_NONE:              return "none";
    case AUDIO_DEVICE_IN_MIC:               return "mic";
    case AUDIO_DEVICE_IN_WIRED_ACCESSORY:   return "wired";
    case AUDIO_DEVICE_IN_BT_SCO:            return "bt_sco";
    default:                                return "invalid";
    }
}

static const char *__get_device_out_str(uint32_t device_out)
{
    switch (device_out) {
    case AUDIO_DEVICE_OUT_NONE:             return "none";
    case AUDIO_DEVICE_OUT_SPEAKER:          return "spk";
    case AUDIO_DEVICE_OUT_RECEIVER:         return "recv";
    case AUDIO_DEVICE_OUT_WIRED_ACCESSORY:  return "wired";
    case AUDIO_DEVICE_OUT_BT_SCO:           return "bt_sco";
    case AUDIO_DEVICE_OUT_BT_A2DP:          return "bt_a2dp";
    case AUDIO_DEVICE_OUT_DOCK:             return "dock";
    case AUDIO_DEVICE_OUT_HDMI:             return "hdmi";
    case AUDIO_DEVICE_OUT_MIRRORING:        return "mirror";
    case AUDIO_DEVICE_OUT_USB_AUDIO:        return "usb";
    case AUDIO_DEVICE_OUT_MULTIMEDIA_DOCK:  return "multi_dock";
    default:                                return "invalid";
    }
}

static void __load_dump_config(struct userdata *u)
{
    dictionary * dict = NULL;
    int vconf_dump = 0;

    dict = iniparser_load(PA_DUMP_INI_DEFAULT_PATH);
    if (!dict) {
        pa_log_debug("%s load failed. Use temporary file", PA_DUMP_INI_DEFAULT_PATH);
        dict = iniparser_load(PA_DUMP_INI_TEMP_PATH);
        if (!dict) {
            pa_log_warn("%s load failed", PA_DUMP_INI_TEMP_PATH);
            return;
        }
    }

    vconf_dump |= iniparser_getboolean(dict, "pcm_dump:decoder_out", 0) ? PA_DUMP_PLAYBACK_DECODER_OUT : 0;
    vconf_dump |= iniparser_getboolean(dict, "pcm_dump:sa_sb_in", 0) ? PA_DUMP_PLAYBACK_SA_SB_IN : 0;
    vconf_dump |= iniparser_getboolean(dict, "pcm_dump:sa_sb_out", 0) ? PA_DUMP_PLAYBACK_SA_SB_OUT : 0;
    vconf_dump |= iniparser_getboolean(dict, "pcm_dump:resampler_in", 0) ? PA_DUMP_PLAYBACK_RESAMPLER_IN : 0;
    vconf_dump |= iniparser_getboolean(dict, "pcm_dump:resampler_out", 0) ? PA_DUMP_PLAYBACK_RESAMPLER_OUT : 0;
    vconf_dump |= iniparser_getboolean(dict, "pcm_dump:dha_out", 0) ? PA_DUMP_PLAYBACK_DHA_IN : 0;
    vconf_dump |= iniparser_getboolean(dict, "pcm_dump:dha_out", 0) ? PA_DUMP_PLAYBACK_DHA_OUT : 0;
    vconf_dump |= iniparser_getboolean(dict, "pcm_dump:gst_avsystem_sink", 0) ? PA_DUMP_PLAYBACK_GST_AVSYSTEM_SINK : 0;
    vconf_dump |= iniparser_getboolean(dict, "pcm_dump:avsystem_write", 0) ? PA_DUMP_PLAYBACK_AVSYSTEM_WRITE : 0;
    vconf_dump |= iniparser_getboolean(dict, "pcm_dump:avsystem_read", 0) ? PA_DUMP_CAPTURE_AVSYSTEM_READ : 0;
    vconf_dump |= iniparser_getboolean(dict, "pcm_dump:gst_avsystem_src", 0) ? PA_DUMP_CAPTURE_GST_AVSYSTEM_SRC : 0;
    vconf_dump |= iniparser_getboolean(dict, "pcm_dump:sec_record_in", 0) ? PA_DUMP_CAPTURE_SEC_RECORD_IN : 0;
    vconf_dump |= iniparser_getboolean(dict, "pcm_dump:sec_record_out", 0) ? PA_DUMP_CAPTURE_SEC_RECORD_OUT : 0;
    vconf_dump |= iniparser_getboolean(dict, "pcm_dump:encoder_in", 0) ? PA_DUMP_CAPTURE_ENCODER_IN : 0;
    u->core->dump_sink = (pa_bool_t)iniparser_getboolean(dict, "pcm_dump:pa_sink", 0);
    u->core->dump_sink_input = (pa_bool_t)iniparser_getboolean(dict, "pcm_dump:pa_sink_input", 0);
    u->core->dump_source = (pa_bool_t)iniparser_getboolean(dict, "pcm_dump:pa_source", 0);
    u->core->dump_source_output = (pa_bool_t)iniparser_getboolean(dict, "pcm_dump:pa_source_output", 0);

    iniparser_freedict(dict);

    if (vconf_set_int(PA_DUMP_VCONF_KEY, vconf_dump)) {
        pa_log_warn("vconf_set_int %s=%x failed", PA_DUMP_VCONF_KEY, vconf_dump);
    }
}

static inline pa_bool_t __is_noise_reduction_on(void)
{
    int noise_reduction_on = 1;

    if (vconf_get_bool(VCONFKEY_CALL_NOISE_REDUCTION_STATE_BOOL, &noise_reduction_on)) {
        pa_log_warn("vconf_get_bool for %s failed", VCONFKEY_CALL_NOISE_REDUCTION_STATE_BOOL);
    }

    return (noise_reduction_on == 1) ? TRUE : FALSE;
}

static bool __is_extra_volume_on(void)
{
    int extra_volume_on = 1;

    if (vconf_get_bool(VCONFKEY_CALL_EXTRA_VOLUME_STATE_BOOL, &extra_volume_on )) {
        pa_log_warn("vconf_get_bool for %s failed", VCONFKEY_CALL_EXTRA_VOLUME_STATE_BOOL);
    }

    return (extra_volume_on == 1) ? TRUE : FALSE;
}

static bool __is_wideband(void)
{
    int wbamr = 1;

    if (vconf_get_bool(VCONFKEY_CALL_WBAMR_STATE_BOOL, &wbamr)) {
        pa_log_warn("vconf_get_bool for %s failed", VCONFKEY_CALL_WBAMR_STATE_BOOL);
    }

    return (wbamr == 1) ? TRUE : FALSE;
}

static pa_bool_t policy_is_bluez (pa_sink* sink)
{
    const char* api_name = NULL;

    if (sink == NULL) {
        pa_log_warn ("input param sink is null");
        return FALSE;
    }

    api_name = pa_proplist_gets(sink->proplist, PA_PROP_DEVICE_API);
    if (api_name) {
        if (pa_streq (api_name, BLUEZ_API))
            return TRUE;
    }

    return FALSE;
}

/* check if this sink is bluez */
static pa_bool_t policy_is_usb_alsa (pa_sink* sink)
{
    const char* api_name = NULL;
    const char* device_bus_name = NULL;

    if (sink == NULL) {
        pa_log_warn ("input param sink is null");
        return FALSE;
    }

    api_name = pa_proplist_gets(sink->proplist, PA_PROP_DEVICE_API);
    if (api_name) {
        if (pa_streq (api_name, ALSA_API)) {
            device_bus_name = pa_proplist_gets(sink->proplist, PA_PROP_DEVICE_BUS);
            if (device_bus_name) {
                if (pa_streq (device_bus_name, "usb"))
                    return TRUE;
            }
        }
    }

    return FALSE;
}

/* Get sink by name */
static pa_sink* policy_get_sink_by_name (pa_core *c, const char* sink_name)
{
    return (pa_sink*)pa_namereg_get(c, sink_name, PA_NAMEREG_SINK);
}

/* Get bt sink if available */
static pa_sink* policy_get_bt_sink (pa_core *c)
{
    pa_sink *s = NULL;
    uint32_t idx;

    if (c == NULL) {
        pa_log_warn ("input param is null");
        return NULL;
    }

    PA_IDXSET_FOREACH(s, c->sinks, idx) {
        if (policy_is_bluez (s)) {
            pa_log_debug_verbose("return [%p] for [%s]\n", s, s->name);
            return s;
        }
    }
    return NULL;
}

/* Select sink for given condition */
static pa_sink* policy_select_proper_sink (struct userdata *u, const char* policy, pa_sink_input *sink_input, pa_bool_t check_bt)
{
    pa_core *c = u->core;
    pa_sink* sink = NULL;
    pa_sink* bt_sink = NULL;
    pa_sink* def = NULL;
    pa_sink_input *si = NULL;
    uint32_t idx;
    int is_mono = 0;
    const char *si_policy_str;
    char *args = NULL;

    pa_assert (u);
    c = u->core;
    is_mono = u->is_mono;
    pa_assert (c);
    if (policy == NULL) {
        pa_log_warn ("input param is null");
        return NULL;
    }

    if (check_bt)
        bt_sink = policy_get_bt_sink(c);
    def = pa_namereg_get_default_sink(c);
    if (def == NULL) {
        pa_log_warn ("pa_namereg_get_default_sink() returns null");
        return NULL;
    }

    /* Select sink to */
    if (pa_streq(policy, POLICY_ALL)) {
        /* all */
        if (bt_sink) {
            if(u->module_combined == NULL) {
                pa_log_info ("combined sink is not prepared, now load-modules...");
                /* load combine sink */
                args = pa_sprintf_malloc("sink_name=%s slaves=\"%s,%s\"", SINK_COMBINED, bt_sink->name, SINK_ALSA);
                u->module_combined = pa_module_load(u->module->core, "module-combine", args);
                pa_xfree(args);

                /* load mono_combine sink */
                args = pa_sprintf_malloc("sink_name=%s master=%s channels=1", SINK_MONO_COMBINED, SINK_COMBINED);
                u->module_mono_combined = pa_module_load(u->module->core, "module-remap-sink", args);
                pa_xfree(args);

                sink = policy_get_sink_by_name(c, (is_mono)? SINK_MONO_COMBINED : SINK_COMBINED);
            }
            sink = policy_get_sink_by_name(c, (is_mono)? SINK_MONO_COMBINED : SINK_COMBINED);
        } else {
            sink = policy_get_sink_by_name (c, (is_mono)? SINK_MONO_ALSA : SINK_ALSA);
        }
    } else if (pa_streq(policy, POLICY_PHONE)) {
        /* phone */
        if (u->subsession == AUDIO_SUBSESSION_RINGTONE)
            sink = policy_get_sink_by_name(c, AEC_SINK);
        if (!sink)
            sink = policy_get_sink_by_name (c, (is_mono)? SINK_MONO_ALSA : SINK_ALSA);
    } else if (pa_streq(policy, POLICY_VOIP)) {
        /* VOIP */
        /* NOTE: Check voip sink first, if not available, use AEC sink */
        sink = policy_get_sink_by_name (c,SINK_VOIP);
        if (sink == NULL) {
            pa_log_info ("VOIP sink is not available, try to use AEC sink");
            sink = policy_get_sink_by_name (c, AEC_SINK);
            if (sink == NULL) {
                pa_log_info ("AEC sink is not available, set to default sink");
                sink = def;
            }
        }
    } else {
        /* auto */
        if (check_bt && policy_is_bluez(def)) {
            sink = (is_mono)? policy_get_sink_by_name (c, SINK_MONO_BT) : def;
        } else if (policy_is_usb_alsa(def)) {
            sink = def;
        } else if (sink_is_hdmi(def)) {
            sink = (is_mono)? policy_get_sink_by_name (c, SINK_MONO_HDMI) : def;
        } else {
            pa_bool_t highlatency_exist = 0;
            if(pa_streq(policy, POLICY_HIGH_LATENCY)) {
                PA_IDXSET_FOREACH(si, c->sink_inputs, idx) {
                    if ((si_policy_str = pa_proplist_gets(si->proplist, PA_PROP_MEDIA_POLICY))) {
                        if (pa_streq(si_policy_str, POLICY_HIGH_LATENCY) && ((is_mono)?sink_is_mono_highlatency(si->sink) : sink_is_highlatency(si->sink))
                            && (sink_input == NULL || sink_input->index != si->index)) {
                            highlatency_exist = 1;
                            break;
                        }
                    }
                }
                if (!highlatency_exist) {
                    sink = (is_mono)? policy_get_sink_by_name (c, SINK_MONO_HIGH_LATENCY) : policy_get_sink_by_name(c, SINK_HIGH_LATENCY);
                }
            }
            if (!sink)
                sink = (is_mono)? policy_get_sink_by_name (c, SINK_MONO_ALSA) : policy_get_sink_by_name(c, SINK_ALSA);
        }
    }

    pa_log_debug_verbose("policy[%s] is_mono[%d] current default[%s] bt_sink[%s] selected_sink[%s]\n",
            policy, is_mono, def->name, (bt_sink)? bt_sink->name:"null", (sink)? sink->name:"null");
    return sink;
}

static pa_bool_t policy_is_filter (pa_sink_input* si)
{
    const char* role = NULL;

    if (si == NULL) {
        pa_log_warn ("input param sink-input is null");
        return FALSE;
    }

    if ((role = pa_proplist_gets(si->proplist, PA_PROP_MEDIA_ROLE))) {
        if (pa_streq(role, "filter"))
            return TRUE;
    }

    return FALSE;
}

/* ---------------------------------------- UCM MANAGER ----------------------------------------*/
static int pa_uc_mgr_init(struct userdata *u)
{
    int retry = 5;
    const int msec = 200;
    char *card = -1;
    int fd = -1;

    u->handle = dlopen(LIB_UC_MGR, RTLD_NOW);
    if (!u->handle) {
        pa_log_error("failed");
        return -1;
    }

    uc_mgr_init_p = dlsym(u->handle, "uc_mgr_init");
    uc_mgr_deinit_p = dlsym(u->handle, "uc_mgr_deinit");
    uc_mgr_set_use_case_p = dlsym(u->handle, "uc_mgr_set_use_case");
    uc_mgr_get_use_case_p = dlsym(u->handle, "uc_mgr_get_use_case");
    uc_mgr_set_modifier_p = dlsym(u->handle, "uc_mgr_set_modifier");

    while(retry--) {
        if(card == -1 && 0 != snd_card_get_name(DEFAULT_CARD_INDEX, &card)) {
            card = -1;
            pa_log_error("snd_card_get_name failed. retry(%d)", retry);
            pa_msleep(msec);
            continue;
        }

        if(uc_mgr_init_p) {
            u->uc_mgr = uc_mgr_init_p(card);
            if(u->uc_mgr != NULL)
            {
                pa_log_debug("uc_mgr initialized. uc_mgr(%p), retry(%d)", u->uc_mgr, retry);
                return 0;
            } else {
                pa_log_debug("snd_card_get_name failed. retry(%d)", retry);
                pa_msleep(msec);
                continue;
            }
        } else {
            pa_log_error("init func is null");
            goto error;
        }
    }

error:
    if ((fd = creat("/tmp/pa_ucm_init_fail", 0644)) != -1)
        close (fd);

    return -1;
}


static int pa_uc_mgr_deinit(struct userdata *u)
{
    struct pa_alsa_sink_info *sink_info = NULL;
    struct pa_alsa_sink_info *sink_info_n = NULL;
    struct pa_alsa_source_info *source_info = NULL;
    struct pa_alsa_source_info *source_info_n = NULL;

    if(u->uc_mgr && uc_mgr_deinit_p)
        uc_mgr_deinit_p(u->uc_mgr);

    if(u->handle)
        dlclose(u->handle);

    PA_LLIST_FOREACH_SAFE(sink_info, sink_info_n, u->alsa_sinks) {
        PA_LLIST_REMOVE(struct pa_alsa_sink_info, u->alsa_sinks, sink_info);
        pa_module_unload(u->core, sink_info->sink, TRUE);
        if(sink_info->mono_sink)
            pa_module_unload(u->core, sink_info->mono_sink, TRUE);
        if (sink_info->pcm_device)
            pa_xfree((void *)sink_info->pcm_device);
        if (sink_info->name)
            pa_xfree(sink_info->name);
        pa_xfree(sink_info);
    }

    PA_LLIST_FOREACH_SAFE(source_info, source_info_n, u->alsa_sources) {
        PA_LLIST_REMOVE(struct pa_alsa_source_info, u->alsa_sources, source_info);
        pa_module_unload(u->core, source_info->source, TRUE);
        if (source_info->pcm_device)
            pa_xfree((void *)source_info->pcm_device);
        if (source_info->name)
            pa_xfree(source_info->name);
        pa_xfree(source_info);
    }

    if(u->snd_playback_pcm)
        snd_pcm_close(u->snd_playback_pcm);
    if(u->snd_capture_pcm)
        snd_pcm_close(u->snd_capture_pcm);

    return 0;
}

static int pa_snd_pcm_set_params(struct userdata *u, snd_pcm_t *pcm)
{
    snd_pcm_hw_params_t *params = NULL;
    int err = 0;
    unsigned int val = 0;

    /* Skip parameter setting to null device. */
    if (snd_pcm_type(pcm) == SND_PCM_TYPE_NULL)
        return -1;

    /* Allocate a hardware parameters object. */
    snd_pcm_hw_params_alloca(&params);

    /* Fill it in with default values. */
    if(snd_pcm_hw_params_any(pcm, params) < 0) {
        pa_log_error("snd_pcm_hw_params_any() : failed! - %s\n", snd_strerror(err));
        goto error;
    }

    /* Set the desired hardware parameters. */
    /* Interleaved mode */
    err = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        pa_log_error("snd_pcm_hw_params_set_access() : failed! - %s\n", snd_strerror(err));
        goto error;
    }

    err = snd_pcm_hw_params_set_rate(pcm, params, (u->wideband?16000:8000), 0);
    if (err < 0) {
        pa_log_error("snd_pcm_hw_params_set_rate() : failed! - %s\n", snd_strerror(err));
    }

    err = snd_pcm_hw_params(pcm, params);
    if (err < 0) {
        pa_log_error("snd_pcm_hw_params() : failed! - %s\n", snd_strerror(err));
        goto error;
    }

    /* Dump current param */
    snd_pcm_hw_params_get_access(params, (snd_pcm_access_t *) &val);
    pa_log_debug("access type = %s\n", snd_pcm_access_name((snd_pcm_access_t)val));

    snd_pcm_hw_params_get_format(params, &val);
    pa_log_debug("format = '%s' (%s)\n",
                    snd_pcm_format_name((snd_pcm_format_t)val),
                    snd_pcm_format_description((snd_pcm_format_t)val));

    snd_pcm_hw_params_get_subformat(params, (snd_pcm_subformat_t *)&val);
    pa_log_debug("subformat = '%s' (%s)\n",
                    snd_pcm_subformat_name((snd_pcm_subformat_t)val),
                    snd_pcm_subformat_description((snd_pcm_subformat_t)val));

    snd_pcm_hw_params_get_channels(params, &val);
    pa_log_debug("channels = %d\n", val);

    return 0;

error:
    return -1;
}

static int pa_snd_pcm_open(struct userdata *u, const char *verb)
{
    const char *playback_pcm_verb = NULL, *capture_pcm_verb = NULL;
    char identifier[50] = {0};

    /* Get playback voice-pcm from ucm conf. Open and set-params */
    snd_pcm_stream_t stream = SND_PCM_STREAM_PLAYBACK;
    sprintf(identifier, "PlaybackPCM/%s", verb);
    uc_mgr_get_use_case_p(u->uc_mgr, identifier, &playback_pcm_verb);
    if (playback_pcm_verb && snd_pcm_open(&u->snd_playback_pcm, playback_pcm_verb, stream, 0) < 0) {
        pa_log_error("snd_pcm_open for %s failed", playback_pcm_verb);
        pa_xfree((void *)playback_pcm_verb);
        return -1;
    }
    pa_snd_pcm_set_params(u, u->snd_playback_pcm);

    pa_log_info("pcm playback device open success device(%s), verb(%s), identifier(%s)", playback_pcm_verb, verb, identifier);

    pa_xfree((void *)playback_pcm_verb);

    memset(identifier, 0, sizeof(identifier));

    /* Get capture voice-pcm from ucm conf. Open and set-params */
    stream = SND_PCM_STREAM_CAPTURE;
    sprintf(identifier, "CapturePCM/%s", verb);
    uc_mgr_get_use_case_p(u->uc_mgr, identifier, &capture_pcm_verb);
    if (capture_pcm_verb && snd_pcm_open(&u->snd_capture_pcm, capture_pcm_verb, stream, 0) < 0) {
        pa_log_error("snd_pcm_open for %s failed", capture_pcm_verb);
        pa_xfree((void *)capture_pcm_verb);
        return -1;
    }
    pa_snd_pcm_set_params(u, u->snd_capture_pcm);

    pa_log_info("pcm captures device open success device(%s), verb(%s), identifier(%s)", capture_pcm_verb, verb, identifier);

    pa_xfree((void *)capture_pcm_verb);

    return 0;
}

static int pa_snd_pcm_close(struct userdata *u)
{
    if(u->snd_playback_pcm) {
        snd_pcm_close(u->snd_playback_pcm);
        u->snd_playback_pcm = NULL;
        pa_log_info("pcm playback device close");
    }

    if(u->snd_capture_pcm) {
        snd_pcm_close(u->snd_capture_pcm);
        u->snd_capture_pcm = NULL;
        pa_log_info("pcm capture device close");
    }

    return 0;
}

#define DEFAULT_FRAGMENT_SIZE 8192
#define DEFAULT_TSCHED_BUFFER_SIZE 16384
#define MAX_STR_LEN 10
#define VOIP_FRAGMENT_SIZE_NB 320
#define VOIP_SAMPLE_RATE_NB 8000
#define VOIP_FRAGMENT_SIZE_WB 640
#define VOIP_SAMPLE_RATE_WB 16000

#define VERB_VOICECALL    "Voice Call"
#define VERB_VIDEOCALL    "Video Call"
#define VERB_LOOPBACK     "Loopback"

/* Start threshold */
#define START_THRESHOLD    4096

#ifdef PA_USE_QCOM_VOIP
#define VOIP_PLAYBACK_START_THRESHOLD_NB 1600
#define VOIP_PLAYBACK_START_THRESHOLD_WB 3200
#define VOIP_CAPTURE_START_THRESHOLD 1
#define VOIP_PCM_DEVICE_STRING "hw:0,3"
#endif

#ifdef PA_USE_QCOM_UCM
static void create_sink(struct userdata *u, const char *playback_pcm)
{
    pa_strbuf *args_buf= NULL;
    char *args = NULL;
    pa_bool_t found = FALSE;
    struct pa_alsa_sink_info *sink_info, *next;
    char name[50] = {0};
    int start_threshold = -1;
    uint32_t channels = CH_STEREO;
    int is_surround = FALSE;
    int hdmi_support = -1;
    int hdmi_support_channels = 0;
    int hdmi_support_channel_max = 0;

    pa_log_debug("playback_pcm = %s", playback_pcm);

    /*  check Device ID LPA hw:0,4 for setting threshold*/
    if (pa_streq(playback_pcm, "hw:0,4")) {
        pa_log_debug("Playback is LPA [%s]", playback_pcm);
        start_threshold = START_THRESHOLD;
    }

    /* Check exist */
    PA_LLIST_FOREACH_SAFE(sink_info, next, u->alsa_sinks) {
        if (pa_streq(sink_info->pcm_device, playback_pcm)) {
            found = TRUE;
            break;
        }
    }

    /* If Not Found, create one */
    if (!found) {
        sink_info = pa_xnew0(struct pa_alsa_sink_info, 1);
        PA_LLIST_INIT(struct pa_alsa_sink_info, sink_info);
        sprintf(name, "alsa_output.%c.analog-stereo", playback_pcm[5]);
        pa_log_debug("sink_name = %s",name);
        sink_info->name = strdup (name);
        args_buf = pa_strbuf_new();

#ifdef PA_USE_QCOM_VOIP
        if (pa_streq(playback_pcm, VOIP_PCM_DEVICE_STRING)) {
            bool is_wideband = __is_wideband();

            pa_strbuf_printf(args_buf,
                "sink_name=\"%s\" "
                "device=\"%s\" "
                "fragment_size=%d "
                "rate=%d "
                "start_threshold=%d "
                "mmap=0 "
                "channels=1 "
                "fragments=1 ",
                name,
                playback_pcm,
                (is_wideband) ? VOIP_FRAGMENT_SIZE_WB : VOIP_FRAGMENT_SIZE_NB,
                (is_wideband) ? VOIP_SAMPLE_RATE_WB : VOIP_SAMPLE_RATE_NB,
                (is_wideband) ? VOIP_PLAYBACK_START_THRESHOLD_WB : VOIP_PLAYBACK_START_THRESHOLD_NB);
        } else if (pa_streq(name, SINK_HDMI)) {
#else
        if (pa_streq(name, SINK_HDMI)) {
#endif
            vconf_get_int(VCONFKEY_SOUND_HDMI_SUPPORT, &hdmi_support);
            pa_log_info("hdmi_support : %X ",  hdmi_support);
            __parse_hdmi_support_info(hdmi_support, &hdmi_support_channels, &hdmi_support_channel_max);
            pa_log_info("support channels : %d, max support channel : %d ", hdmi_support_channels, hdmi_support_channel_max);
            if(hdmi_support_channel_max == 2){
                channels = 2;
                pa_log_info("This HDMI device supports only %dch", channels);
            }
            else if(hdmi_support_channel_max > 2){
                vconf_get_int(VCONFKEY_SETAPPL_ACCESSORY_AUDIO_OUTPUT, &is_surround);
                if(is_surround){
                    channels = hdmi_support_channel_max;
                    pa_log_info("Setting for Surrorund is ON, load HDMI as %.1fch", hdmi_support_channel_max==6 ? 5.1 : 7.1);
                }
                else{
                    channels = 2;
                    pa_log_info("Setting for Surround is OFF, load HDMI as %dch", channels);
                }
            }
            else{
                channels = 2;
                pa_log_info("set default HDMI channel : %d", channels);
            }
            pa_strbuf_printf(args_buf,
                "sink_name=\"%s\" "
                "device=\"%s\" "
                "channels=%d "
                "sink_properties=\"module-suspend-on-idle.timeout=0\" "
                "start_threshold=%d ",
                name,
                playback_pcm,
                channels,
                start_threshold);
        } else {
            pa_strbuf_printf(args_buf,
                "sink_name=\"%s\" "
                "device=\"%s\" "
                "channels=%d "
                "tsched_buffer_size=%d "
                "sink_properties=\"module-suspend-on-idle.timeout=0\" "
                "start_threshold=%d ",
                name,
                playback_pcm,
                channels,
                (u->tsched_buffer_size)? u->tsched_buffer_size : DEFAULT_TSCHED_BUFFER_SIZE,start_threshold);
        }

        args = pa_strbuf_tostring_free(args_buf);
        sink_info->sink = pa_module_load(u->core, "module-alsa-sink", args);
        if (sink_info->sink) {
            pa_log_debug("Successfully Loaded module-alsa-sink: %s",name);

            /* Add to List */
            sink_info->pcm_device = playback_pcm;

            PA_LLIST_PREPEND(struct pa_alsa_sink_info, u->alsa_sinks, sink_info);

            /* Create mono sink for HDMI, LPA and Default sinks */
            if (pa_streq(name, SINK_HDMI)
                || pa_streq(name, SINK_HIGH_LATENCY)
                || pa_streq(name, SINK_ALSA)) {
                pa_xfree(args);
                if (pa_streq(name, SINK_HDMI))
                    args = pa_sprintf_malloc("sink_name=%s master=%s channels=1", SINK_MONO_HDMI, SINK_HDMI);
                else if (pa_streq(name, SINK_HIGH_LATENCY)) {
                    args = pa_sprintf_malloc("sink_name=%s master=%s channels=1", SINK_MONO_HIGH_LATENCY, SINK_HIGH_LATENCY);
                }
                else if (pa_streq(name, SINK_ALSA))
                    args = pa_sprintf_malloc("sink_name=%s master=%s channels=1", SINK_MONO_ALSA, SINK_ALSA);

                sink_info->mono_sink = pa_module_load(u->core, "module-remap-sink", args);
                if (sink_info->mono_sink)
                    pa_log_debug("Successfully Loaded mono sink %s", name);
                else
                    pa_log_error("Failed to Load module-remap-sink: %s",name);
            }

        } else {
            pa_log_error("Failed to Load module-alsa-sink: %s",name);
            if (sink_info->name)
                free (sink_info->name);
            pa_xfree(sink_info);
        }
    } else {
        pa_log_debug("Found!!!!");
    }

    if (args)
        pa_xfree(args);
}

static void create_source(struct userdata *u, const char *capture_pcm)
{
    pa_strbuf *args_buf = NULL;
    char *args = NULL;
    pa_bool_t found = FALSE;
    struct pa_alsa_source_info *source_info, *next;
    char name[50] = {0};
    pa_source *source = NULL;
    char capture_pcm_device;
    int start_threshold = -1;

    pa_log_debug("capture_pcm = %s", capture_pcm);

    /* capture pcm should be [hw:0,X] */
    capture_pcm_device = capture_pcm[5];

    /* Check exist */
    PA_LLIST_FOREACH_SAFE(source_info, next, u->alsa_sources) {
        if (pa_streq(source_info->pcm_device, capture_pcm)) {
            found = TRUE;
            break;
        }
    }

    /* If Not Found, create one */
    if (!found) {
        source_info = pa_xnew0(struct pa_alsa_source_info, 1);
        PA_LLIST_INIT(struct pa_alsa_source_info, source_info);
        sprintf(name, "alsa_input.%c.analog-stereo", capture_pcm[5]);
        pa_log_debug("source_name = %s",name);
        source_info->name = strdup (name);

        args_buf = pa_strbuf_new();

#ifdef PA_USE_QCOM_VOIP
        if (pa_streq(capture_pcm, VOIP_PCM_DEVICE_STRING)) {
            bool is_wideband = __is_wideband();

            pa_strbuf_printf(args_buf,
                "source_name=\"%s\" "
                "device=\"%s\" "
                "fragment_size=%d "
                "rate=%d "
                "start_threshold=%d "
                "mmap=0 "
                "channels=1 "
                "fragments=1 ",
                name,
                capture_pcm,
                (is_wideband) ? VOIP_FRAGMENT_SIZE_WB : VOIP_FRAGMENT_SIZE_NB,
                (is_wideband) ? VOIP_SAMPLE_RATE_WB : VOIP_SAMPLE_RATE_NB,
                VOIP_CAPTURE_START_THRESHOLD);
        } else {
#endif
            pa_strbuf_printf(args_buf,
                         "source_name=\"%s\" "
                         "device=\"%s\" "
                         "fragment_size=%d "
                         "mmap=0 "
                         "start_threshold=%d",
                         name,
                         capture_pcm,
                         (u->fragment_size)? u->fragment_size : DEFAULT_FRAGMENT_SIZE,start_threshold);
#ifdef PA_USE_QCOM_VOIP
        }
#endif

        args = pa_strbuf_tostring_free(args_buf);
        source_info->source = pa_module_load(u->core, "module-alsa-source", args);

        if (source_info->source) {
            pa_log_debug("Successfully Loaded module-alsa-source: %s",name);

            /* Add to List */
            source_info->pcm_device = capture_pcm;
            PA_LLIST_PREPEND(struct pa_alsa_source_info, u->alsa_sources, source_info);

            /* If default mic device, set as default source */
            if (capture_pcm_device == PCM_DEV_MIC) {
                source = pa_namereg_get(u->core, source_info->name, PA_NAMEREG_SOURCE);
                pa_namereg_set_default_source(u->core, source);
            }
        } else {
            pa_log_error("Failed to Loaded module-alsa-source: %s",name);
            if (source_info->name)
                free (source_info->name);
            pa_xfree(source_info);
        }
#ifdef PA_USE_QCOM_VOIP
        /*Loading null-source */
        if(pa_streq(capture_pcm, VOIP_PCM_DEVICE_STRING) && !pa_namereg_get(u->core, NULL_SOURCE, PA_NAMEREG_SOURCE))
        {
           struct pa_alsa_source_info *source_info_null = NULL;
           source_info_null = pa_xnew0(struct pa_alsa_source_info, 1);
           PA_LLIST_INIT(struct pa_alsa_source_info, source_info_null);
           source_info_null->name = "null";
           source_info_null->pcm_device = "null";
           source_info_null->source = pa_module_load(u->core, "module-null-source", NULL);

           if(source_info_null->source)
              PA_LLIST_PREPEND(struct pa_alsa_source_info, u->alsa_sources, source_info_null);
           else {
              pa_log_error("Failed to Loaded module-null-source ");
              pa_xfree(source_info_null);
           }
        }
#endif
    }

    if (args)
        pa_xfree(args);
}

static void prepare_destination (struct userdata *u, const char *verb, const char *devices[], const char *modifiers[])
{
    int i = 0;
    const char *playback_pcm_verb = NULL, *capture_pcm_verb = NULL;
    char identifier[50] = {0};

    /* CapturePCM for verb */
    sprintf(identifier, "CapturePCM/%s",verb);
    uc_mgr_get_use_case_p(u->uc_mgr, identifier, &capture_pcm_verb);
    if (capture_pcm_verb) {
        create_source(u, capture_pcm_verb);
    }

    /* PlaybackPCM for verb */
    sprintf(identifier, "PlaybackPCM/%s",verb);
    uc_mgr_get_use_case_p(u->uc_mgr, identifier, &playback_pcm_verb);
    if (playback_pcm_verb) {
        create_sink(u, playback_pcm_verb);
    }

    /* Iterate modifiers for PCM devices */
    for (i = 0; modifiers[i]; i++) {
        const char *playback_pcm_mod = NULL, *capture_pcm_mod = NULL;
        int is_capture_mod = strstr(modifiers[i], "Capture") ? 1 : 0;

        /* PlaybackPCM for modifiers */
        if (is_capture_mod == 0) {
            sprintf(identifier, "PlaybackPCM/%s",modifiers[i]);
            uc_mgr_get_use_case_p(u->uc_mgr, identifier, &playback_pcm_mod);
            if (playback_pcm_mod) {
                create_sink(u, playback_pcm_mod);
            }
        }

        /* CapturePCM for modifiers */
        if (is_capture_mod == 1) {
            sprintf(identifier, "CapturePCM/%s",modifiers[i]);
            uc_mgr_get_use_case_p(u->uc_mgr, identifier, &capture_pcm_mod);
            if (capture_pcm_mod) {
                create_source(u, capture_pcm_mod);
            }
        }
    }
}

static int pa_uc_mgr_set_use_case(struct userdata *u, const char *verb, const char *devices[], const char *modifiers[])
{
    audio_return_t audio_ret = AUDIO_RET_OK;
    int result = 0;
    const char *current_verb = NULL;
    pa_sink_input *si = NULL;
    uint32_t idx;
    const char *device_switching_str;
    uint32_t device_switching = 0;
    uint32_t need_sleep_for_ucm = 0;

    if (!u->uc_mgr)
        return -1;

    /* Check interfaces */
    if (uc_mgr_get_use_case_p == NULL || uc_mgr_set_use_case_p == NULL) {
        pa_log_error ("ucm mgr interfaces are not valid...");
        return -1;
    }

    /* Get Current Verb */
    uc_mgr_get_use_case_p(u->uc_mgr, "_verb", &current_verb);

    pa_log_info("verb : current [%s] => new [%s]", current_verb, verb);

    /* Mute sink inputs which are unmuted */
    PA_IDXSET_FOREACH(si, u->core->sink_inputs, idx) {
        if ((device_switching_str = pa_proplist_gets(si->proplist, "module-policy.device_switching"))) {
            pa_atou(device_switching_str, &device_switching);
            if (device_switching) {
                if (AUDIO_IS_ERROR((audio_ret = policy_set_mute(u, si->index, (uint32_t)-1, AUDIO_DIRECTION_OUT, 1)))) {
                    pa_log_warn("policy_set_mute(1) for stream[%d] returns error:0x%x", si->index, audio_ret);
                }
                need_sleep_for_ucm = 1;
            }
        }
    }

    /* FIXME : sleep for ucm */
    if (need_sleep_for_ucm) {
        usleep(150000);
    }

    if (verb) { /* Normal Case */
        if (pa_streq(current_verb, verb) && (pa_streq(verb, VERB_VOICECALL) || pa_streq(verb, VERB_VIDEOCALL))) {  // except voicecall -> voicecall
            pa_log_warn("verb : [%s] => [%s] change to same verb(voice/video call). skip pcm close.", current_verb, verb);
        } else {
            pa_snd_pcm_close(u);
        }

        /* setting path */
        result = uc_mgr_set_use_case_p(u->uc_mgr, verb, devices, modifiers);

        if (verb) {
            /* Open DAI for voice/video call, otherwise Load Sink/Source if needed */
            if (pa_streq(verb, VERB_LOOPBACK)) {
                result = pa_snd_pcm_open(u, verb);
            } else if (pa_streq(verb, VERB_VOICECALL) || pa_streq(verb, VERB_VIDEOCALL)) {
                if (!pa_streq(current_verb, verb)) { // except voicecall -> voicecall
                    result = pa_snd_pcm_open(u, verb);
                }
            } else {
                pa_snd_pcm_close(u);
                /* create sink and source */
                prepare_destination(u, verb, devices, modifiers);
            }
        }
    } else { /* VoiceControl Case : setting path */
        result = uc_mgr_set_use_case_p(u->uc_mgr, verb, devices, modifiers);
        if (u->audio_mgr.set_ecrx_device) { // set EC_REF mixer
            u->audio_mgr.set_ecrx_device(u->audio_mgr.data);
        }
    }

    /* Unmute sink inputs which are muted due to device switching */
    PA_IDXSET_FOREACH(si, u->core->sink_inputs, idx) {
        if ((device_switching_str = pa_proplist_gets(si->proplist, "module-policy.device_switching"))) {
            pa_atou(device_switching_str, &device_switching);
            if (device_switching) {
                pa_proplist_sets(si->proplist, "module-policy.device_switching", "0");
                if (AUDIO_IS_ERROR((audio_ret = __update_volume(u, si->index, (uint32_t)-1, (uint32_t)-1)))) {
                    pa_log_warn("__update_volume for stream[%d] returns error:0x%x", si->index, audio_ret);
                }
                if (AUDIO_IS_ERROR((audio_ret = policy_set_mute(u, si->index, (uint32_t)-1, AUDIO_DIRECTION_OUT, 0)))) {
                    pa_log_warn("policy_set_mute(0) for stream[%d] returns error:0x%x", si->index, audio_ret);
                }
            }
        }
    }

    pa_log_debug("result = %d", result);

    return result;
}
/* ---------------------------------------- UCM MANAGER : END ----------------------------------------*/
#endif // PA_USE_QCOM_UCM

static pa_sink *__get_real_master_sink(pa_sink_input *si)
{
    const char *master_name;
    pa_sink *s, *sink;

    s = (si->origin_sink) ? si->origin_sink : si->sink;
    master_name = pa_proplist_gets(s->proplist, PA_PROP_DEVICE_MASTER_DEVICE);
    if (master_name)
        sink = pa_namereg_get(si->core, master_name, PA_NAMEREG_SINK);
    else
        sink = s;
    return sink;
}

static audio_return_t __fill_audio_stream_info(pa_proplist *sink_input_proplist, pa_sample_spec *sample_spec, audio_info_t *audio_info)
{
    const char *si_volume_type_str, *si_gain_type_str;

    if (!sink_input_proplist) {
        return AUDIO_ERR_PARAMETER;
    }

    audio_info->stream.name = pa_strnull(pa_proplist_gets(sink_input_proplist, PA_PROP_MEDIA_NAME));
    audio_info->stream.samplerate = sample_spec->rate;
    audio_info->stream.channels = sample_spec->channels;
    audio_info->stream.gain_type = AUDIO_GAIN_TYPE_DEFAULT;

    /* Get volume type of sink input */
    if ((si_volume_type_str = pa_proplist_gets(sink_input_proplist, PA_PROP_MEDIA_TIZEN_VOLUME_TYPE))) {
        pa_atou(si_volume_type_str, &audio_info->stream.volume_type);
    } else {
        return AUDIO_ERR_UNDEFINED;
    }

    /* Get gain type of sink input */
    if ((si_gain_type_str = pa_proplist_gets(sink_input_proplist, PA_PROP_MEDIA_TIZEN_GAIN_TYPE))) {
        pa_atou(si_gain_type_str, &audio_info->stream.gain_type);
    }

    return AUDIO_RET_OK;
}

static audio_return_t __fill_audio_device_info(pa_proplist *sink_proplist, audio_info_t *audio_info)
{
    const char *s_device_api_str;

    if (!sink_proplist) {
        return AUDIO_ERR_PARAMETER;
    }

    audio_info->device.api = AUDIO_DEVICE_API_UNKNOWN;

    /* Get device api */
    if ((s_device_api_str = pa_proplist_gets(sink_proplist, PA_PROP_DEVICE_API))) {
        if (pa_streq(s_device_api_str, "alsa")) {
            const char *card_idx_str, *device_idx_str;

            audio_info->device.api = AUDIO_DEVICE_API_ALSA;
            audio_info->device.alsa.card_idx = 0;
            audio_info->device.alsa.device_idx = 0;
            if ((card_idx_str = pa_proplist_gets(sink_proplist, "alsa.card")))
                pa_atou(card_idx_str, &audio_info->device.alsa.card_idx);
            if ((device_idx_str = pa_proplist_gets(sink_proplist, "alsa.device")))
                pa_atou(device_idx_str, &audio_info->device.alsa.device_idx);
        }
        else if (pa_streq(s_device_api_str, "bluez")) {
            const char *nrec_str;

            audio_info->device.api = AUDIO_DEVICE_API_BLUEZ;
            audio_info->device.bluez.nrec = 0;
            audio_info->device.bluez.protocol = (char *)pa_strnull(pa_proplist_gets(sink_proplist, "bluetooth.protocol"));
            if ((nrec_str = pa_proplist_gets(sink_proplist, "bluetooth.nrec")))
                pa_atou(nrec_str, &audio_info->device.bluez.nrec);
        }
    }

    return AUDIO_RET_OK;
}

static audio_return_t __fill_audio_info(pa_sink_input *si, audio_info_t *audio_info)
{
    audio_return_t audio_ret = AUDIO_RET_OK;
    pa_sink *sink;

    sink = __get_real_master_sink(si);
    if (AUDIO_IS_ERROR((audio_ret = __fill_audio_stream_info(si->proplist, &si->sample_spec, audio_info)))) {
        return audio_ret;
    }
    if (AUDIO_IS_ERROR((audio_ret = __fill_audio_device_info(sink->proplist, audio_info)))) {
        return audio_ret;
    }

    return AUDIO_RET_OK;
}

#define BURST_SOUND_DEFAULT_TIME_INTERVAL (0.09 * PA_USEC_PER_SEC)
static void __play_audio_sample_timeout_cb(pa_mainloop_api *m, pa_time_event *e, const struct timeval *t, void *userdata)
{
    struct userdata* u = (struct userdata*)userdata;
    pa_usec_t interval = u->audio_sample_userdata.time_interval;
    pa_usec_t now = 0ULL;

    pa_assert(m);
    pa_assert(e);
    pa_assert(u);

    pa_mutex_lock(u->audio_sample_userdata.mutex);
    if(u->audio_sample_userdata.is_running) {
        // calculate timer boosting
        if(u->audio_sample_userdata.factor > 1ULL)
            interval = u->audio_sample_userdata.time_interval / u->audio_sample_userdata.factor;

        if(u->audio_sample_userdata.count == 0) {
            // 5. first post data
            pa_sink_input_put(u->audio_sample_userdata.i);
        } else {
            // 5. post data
            if(pa_memblockq_push(u->audio_sample_userdata.q, &u->audio_sample_userdata.e->memchunk) < 0) {
                pa_log_error("memory push fail cnt(%d), factor(%llu), interval(%llu)",
                    u->audio_sample_userdata.count, u->audio_sample_userdata.factor, u->audio_sample_userdata.time_interval);
                pa_assert(0);
            }
        }
        u->audio_sample_userdata.count++;

        pa_rtclock_now_args(&now);
        pa_core_rttime_restart(u->core, e, now + interval);
        if(u->audio_sample_userdata.factor > 1ULL)
            u->audio_sample_userdata.factor -= 1ULL;
    } else {
        pa_core_rttime_restart(u->core, e, PA_USEC_INVALID);
        u->core->mainloop->time_free(u->audio_sample_userdata.time_event);

        // fading. but should be emitted first sutter sound totally.
        if(u->audio_sample_userdata.count > 1) {
            pa_sink_input_set_mute(u->audio_sample_userdata.i, TRUE, TRUE);
            pa_sink_input_unlink(u->audio_sample_userdata.i);
        }
        pa_sink_input_unref(u->audio_sample_userdata.i);
    }
    pa_mutex_unlock(u->audio_sample_userdata.mutex);
}

static audio_return_t policy_play_sample_continuously(struct userdata *u, pa_native_connection *c, const char *name, pa_usec_t interval,
    uint32_t volume_type, uint32_t gain_type, uint32_t volume_level, uint32_t *stream_idx)
{
    audio_return_t audio_ret = AUDIO_RET_OK;
    pa_proplist *p = 0;
    pa_sink *sink = NULL;
    audio_info_t audio_info;
    double volume_linear = 1.0f;
    pa_client *client = pa_native_connection_get_client(c);

    pa_scache_entry *e;
    pa_bool_t pass_volume = TRUE;
    pa_proplist *merged =0;
    pa_sink_input* i = NULL;
    pa_memblockq *q;
    pa_memchunk silence;
    pa_cvolume r;
    pa_usec_t now = 0ULL;

    if(!u->audio_sample_userdata.mutex)
        u->audio_sample_userdata.mutex = pa_mutex_new(FALSE, FALSE);

    pa_mutex_lock(u->audio_sample_userdata.mutex);

    pa_assert(u->audio_sample_userdata.is_running == FALSE); // allow one instace.

    memset(&audio_info, 0x00, sizeof(audio_info_t));

    p = pa_proplist_new();

    /* Set volume type of stream */
    pa_proplist_setf(p, PA_PROP_MEDIA_TIZEN_VOLUME_TYPE, "%d", volume_type);
    /* Set gain type of stream */
    pa_proplist_setf(p, PA_PROP_MEDIA_TIZEN_GAIN_TYPE, "%d", gain_type);
    /* Set policy */
    pa_proplist_setf(p, PA_PROP_MEDIA_POLICY, "%s", volume_type == AUDIO_VOLUME_TYPE_FIXED ? POLICY_PHONE : POLICY_AUTO);

    pa_proplist_update(p, PA_UPDATE_MERGE, client->proplist);

    sink = pa_namereg_get_default_sink(u->core);

    /* FIXME : Add gain_type parameter to API like volume_type */
    audio_info.stream.gain_type = gain_type;

    if (AUDIO_IS_ERROR((audio_ret = u->audio_mgr.get_volume_value(u->audio_mgr.data, &audio_info, volume_type, volume_level, &volume_linear)))) {
        pa_log_warn("get_volume_value returns error:0x%x", audio_ret);
        goto exit;
    }

   /*
    1. load cam-shutter sample
    2. create memchunk using sample.
    3. create sink_input(cork mode)
    4. set timer
    5. post data(sink-input put or push memblockq)
    */

    //  1. load cam-shutter sample
    merged = pa_proplist_new();

    if (!(e = pa_namereg_get(u->core, name, PA_NAMEREG_SAMPLE)))
        goto exit;

    pa_proplist_sets(merged, PA_PROP_MEDIA_NAME, name);
    pa_proplist_sets(merged, PA_PROP_EVENT_ID, name);

    if (e->lazy && !e->memchunk.memblock) {
        pa_channel_map old_channel_map = e->channel_map;

        if (pa_sound_file_load(u->core->mempool, e->filename, &e->sample_spec, &e->channel_map, &e->memchunk, merged) < 0)
            goto exit;

        pa_subscription_post(u->core, PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE|PA_SUBSCRIPTION_EVENT_CHANGE, e->index);

        if (e->volume_is_set) {
            if (pa_cvolume_valid(&e->volume))
                pa_cvolume_remap(&e->volume, &old_channel_map, &e->channel_map);
            else
                pa_cvolume_reset(&e->volume, e->sample_spec.channels);
        }
    }

    if (!e->memchunk.memblock)
        goto exit;

    if (e->volume_is_set && PA_VOLUME_IS_VALID(pa_sw_volume_from_linear(volume_linear))) {
        pa_cvolume_set(&r, e->sample_spec.channels, pa_sw_volume_from_linear(volume_linear));
        pa_sw_cvolume_multiply(&r, &r, &e->volume);
    } else if (e->volume_is_set)
        r = e->volume;
    else if (PA_VOLUME_IS_VALID(pa_sw_volume_from_linear(volume_linear)))
        pa_cvolume_set(&r, e->sample_spec.channels, pa_sw_volume_from_linear(volume_linear));
    else
        pass_volume = FALSE;

    pa_proplist_update(merged, PA_UPDATE_MERGE, e->proplist);
    pa_proplist_update(p, PA_UPDATE_MERGE, merged);

    if (e->lazy)
        time(&e->last_used_time);

    // 2. create memchunk using sample.
    pa_silence_memchunk_get(&sink->core->silence_cache, sink->core->mempool, &silence, &e->sample_spec, 0);
    q = pa_memblockq_new("pa_play_memchunk() q", 0, e->memchunk.length * 35, 0, &e->sample_spec, 1, 1, 0, &silence);
    pa_memblock_unref(silence.memblock);

    pa_assert_se(pa_memblockq_push(q, &e->memchunk) >= 0);

    // 3. create sink_input(cork mode)
    if (!(i = pa_memblockq_sink_input_new(sink, &e->sample_spec, &e->channel_map, q, pass_volume ? &r : NULL,
        p, PA_SINK_INPUT_NO_CREATE_ON_SUSPEND|PA_SINK_INPUT_KILL_ON_SUSPEND)))
        goto exit;

    // 4. set timer
    u->audio_sample_userdata.e = e;
    u->audio_sample_userdata.i = i;
    u->audio_sample_userdata.q = q;
    u->audio_sample_userdata.time_interval = interval == (pa_usec_t)0 ? BURST_SOUND_DEFAULT_TIME_INTERVAL : interval;
    u->audio_sample_userdata.is_running = TRUE;
    u->audio_sample_userdata.factor = 4ULL; // for memory block boosting
    u->audio_sample_userdata.count = 0;

    pa_rtclock_now_args(&now); // doesn't use arm barrel shiter. SBF
    pa_log_warn("now(%llu), start interval(%llu)", now, interval / u->audio_sample_userdata.factor);
    u->audio_sample_userdata.factor -= 1ULL;
    u->audio_sample_userdata.time_event = pa_core_rttime_new(u->core, now, __play_audio_sample_timeout_cb, u);

exit:
    if(p)
        pa_proplist_free(p);
    if(merged)
        pa_proplist_free(merged);

    pa_mutex_unlock(u->audio_sample_userdata.mutex);

    return audio_ret;
}

static void  policy_stop_sample_continuously(struct userdata *u)
{
    if(u->audio_sample_userdata.time_event) {
        pa_mutex_lock(u->audio_sample_userdata.mutex);
        pa_assert(u->audio_sample_userdata.is_running);
        u->audio_sample_userdata.is_running = FALSE;
        pa_mutex_unlock(u->audio_sample_userdata.mutex);
        pa_log_info("timeout_cb called (%d) times", u->audio_sample_userdata.count);
    }
}

static audio_return_t policy_play_sample(struct userdata *u, pa_native_connection *c, const char *name, uint32_t volume_type, uint32_t gain_type, uint32_t volume_level, uint32_t *stream_idx)
{
    audio_return_t audio_ret = AUDIO_RET_OK;
    pa_proplist *p;
    pa_sink *sink = NULL;
    audio_info_t audio_info;
    double volume_linear = 1.0f;
    pa_client *client = pa_native_connection_get_client(c);
    pa_bool_t is_boot_sound;
    uint32_t sample_idx = 0;
    char* booting = NULL;
    char* file_to_add = NULL;
    int sample_ret = 0;

    memset(&audio_info, 0x00, sizeof(audio_info_t));

    p = pa_proplist_new();

    /* Set volume type of stream */
    pa_proplist_setf(p, PA_PROP_MEDIA_TIZEN_VOLUME_TYPE, "%d", volume_type);
    /* Set gain type of stream */
    pa_proplist_setf(p, PA_PROP_MEDIA_TIZEN_GAIN_TYPE, "%d", gain_type);
    /* Set policy */
    pa_proplist_setf(p, PA_PROP_MEDIA_POLICY, "%s", volume_type == AUDIO_VOLUME_TYPE_FIXED ? POLICY_PHONE : POLICY_AUTO);

    pa_proplist_update(p, PA_UPDATE_MERGE, client->proplist);

    sink = pa_namereg_get_default_sink(u->core);

    /* FIXME : Add gain_type parameter to API like volume_type */
    audio_info.stream.gain_type = gain_type;

    if (AUDIO_IS_ERROR((audio_ret = u->audio_mgr.get_volume_value(u->audio_mgr.data, &audio_info, volume_type, volume_level, &volume_linear)))) {
        pa_log_warn("get_volume_value returns error:0x%x", audio_ret);
        goto exit;
    }

    pa_log_debug("play_sample volume_type:%d gain_type:%d volume_linear:%f", volume_type, gain_type, volume_linear);

    is_boot_sound = pa_streq(name, BOOTING_SOUND_SAMPLE);
    if (is_boot_sound && pa_namereg_get(u->core, name, PA_NAMEREG_SAMPLE) == NULL) {
        booting = vconf_get_str(VCONF_BOOTING);
        file_to_add = (booting)? booting : DEFAULT_BOOTING_SOUND_PATH;
        if ((sample_ret = pa_scache_add_file(u->core, name, file_to_add, &sample_idx)) != 0) {
            pa_log_error("failed to add sample [%s][%s]", name, file_to_add);
        } else {
            pa_log_info("success to add sample [%s][%s]", name, file_to_add);
        }
        if (booting)
            free(booting);
    }

    if (pa_scache_play_item(u->core, name, sink, pa_sw_volume_from_linear(volume_linear), p, stream_idx) < 0) {
        pa_log_error("pa_scache_play_item fail");
        audio_ret = AUDIO_ERR_UNDEFINED;
        goto exit;
    }

    if (is_boot_sound && sample_ret == 0) {
        if (pa_scache_remove_item(u->core, name) != 0) {
            pa_log_error("failed to remove sample [%s]", name);
        } else {
            pa_log_info("success to remove sample [%s]", name);
        }
    }

exit:
    pa_proplist_free(p);

    return audio_ret;
}

static audio_return_t policy_reset(struct userdata *u)
{
    audio_return_t audio_ret = AUDIO_RET_OK;

    pa_log_debug("reset");

    __load_dump_config(u);

    if (u->audio_mgr.reset) {
        if (AUDIO_IS_ERROR((audio_ret = u->audio_mgr.reset(&u->audio_mgr.data)))) {
            pa_log_error("audio_mgr reset failed");
            return audio_ret;
        }
    }

    return audio_ret;
}

static audio_return_t policy_set_session(struct userdata *u, uint32_t session, uint32_t start) {

    pa_log_info("set_session:%s %s (current:%s,%s)",
            __get_session_str(session), (start) ? "start" : "end", __get_session_str(u->session), __get_subsession_str(u->subsession));

    if (start) {
        u->session = session;

        if ((u->session == AUDIO_SESSION_VOICECALL) || (u->session == AUDIO_SESSION_VIDEOCALL)) {
            u->subsession = AUDIO_SUBSESSION_MEDIA;
            u->call_muted = 0;
        } else if (u->session == AUDIO_SESSION_VOIP) {
            u->subsession = AUDIO_SUBSESSION_VOICE;
        } else if (u->session == AUDIO_SESSION_VOICE_RECOGNITION) {
            u->subsession = AUDIO_SUBSESSION_VR_INIT;
        } else {
            u->subsession = AUDIO_SUBSESSION_NONE;
        }
    } else {
           if(u->session ==  AUDIO_SESSION_VIDEOCALL || u->session ==  AUDIO_SESSION_VOIP) {
              voip_modules_unloading(u);
        }
        u->session = AUDIO_SESSION_MEDIA;
        u->subsession = AUDIO_SUBSESSION_NONE;
    }

    if (u->audio_mgr.set_session) {
        u->audio_mgr.set_session(u->audio_mgr.data, u->session, u->subsession);
    }

    /* route should be updated */
    u->active_route_flag = (uint32_t)-1;

    return AUDIO_RET_OK;
}

static void voip_modules_unloading(struct userdata *u)
{
     struct pa_alsa_sink_info *sink_info = NULL;
     struct pa_alsa_sink_info *sink_info_n = NULL;
     struct pa_alsa_source_info *source_info = NULL;
     struct pa_alsa_source_info *source_info_n = NULL;
     uint32_t idx = 0;
     pa_sink* sink_tmp = NULL;
     pa_source* source_tmp = NULL;

     PA_LLIST_FOREACH_SAFE(sink_info, sink_info_n, u->alsa_sinks) {
         if (sink_info->name && pa_streq(sink_info->name, SINK_VOIP)) {
             pa_log_info("VOIP sink[%s] found ", sink_info->name);
             sink_tmp = (pa_sink *)pa_namereg_get(u->core, sink_info->name, PA_NAMEREG_SINK);
             if(pa_sink_check_suspend(sink_tmp))
             {
                pa_sink_input *i = NULL;
                pa_sink *sink_null = NULL;
                sink_null = (pa_sink *)pa_namereg_get(u->core, "null", PA_NAMEREG_SINK);
                pa_log_info("Moving all the sink-input connected to voip sink to null-sink");
                PA_IDXSET_FOREACH(i, sink_tmp->inputs, idx) {
                    pa_sink_input_move_to(i, sink_null, TRUE);
                }
             }
             pa_sink_suspend(sink_tmp,TRUE,PA_SUSPEND_APPLICATION);
             PA_LLIST_REMOVE(struct pa_alsa_sink_info, u->alsa_sinks, sink_info);
             pa_module_unload(u->core, sink_info->sink, TRUE);
             pa_xfree(sink_info->name);
             pa_xfree(sink_info);
             break;
         }
      }
      PA_LLIST_FOREACH_SAFE(source_info, source_info_n, u->alsa_sources) {
          if (source_info->name && pa_streq(source_info->name, SOURCE_VOIP)) {
              pa_log_info("VOIP source [%s] found ", source_info->name);
              source_tmp = (pa_source *)pa_namereg_get(u->core, source_info->name, PA_NAMEREG_SOURCE);
              if (pa_source_check_suspend(source_tmp))
              {
                 pa_source_output *o = NULL;
                 pa_source *source_null = NULL;
                 source_null = (pa_source *)pa_namereg_get(u->core, NULL_SOURCE, PA_NAMEREG_SOURCE);
                 pa_log_info("Moving all the source-output connected to voip source to null-source");
                 PA_IDXSET_FOREACH(o, source_tmp->outputs, idx) {
                     pa_source_output_move_to(o, source_null, TRUE);
                 }
              }
              pa_source_suspend(source_tmp,TRUE,PA_SUSPEND_APPLICATION);
              PA_LLIST_REMOVE(struct pa_alsa_source_info, u->alsa_sources, source_info);
              pa_module_unload(u->core, source_info->source, TRUE);
              pa_xfree(source_info->name);
              pa_xfree(source_info);
              break;
          }
       }
}

static audio_return_t policy_set_subsession(struct userdata *u, uint32_t subsession, uint32_t subsession_opt) {

    pa_log_info("set_subsession:%s->%s opt:%x->%x (session:%s)",
            __get_subsession_str(u->subsession), __get_subsession_str(subsession), u->subsession_opt, subsession_opt,
            __get_session_str(u->session));

    if (u->subsession == subsession && u->subsession_opt == subsession_opt) {
        pa_log_debug("duplicated request is ignored subsession(%d) opt(0x%x)", subsession, subsession_opt);
        return AUDIO_RET_OK;
    }

    u->subsession = subsession;
    if (u->subsession == AUDIO_SUBSESSION_VR_NORMAL || u->subsession == AUDIO_SUBSESSION_VR_DRIVE)
        u->subsession_opt = subsession_opt;
    else
        u->subsession_opt = 0;

    if (u->audio_mgr.set_session) {
        u->audio_mgr.set_session(u->audio_mgr.data, u->session, u->subsession);
    }

    /* route should be updated */
    u->active_route_flag = (uint32_t)-1;

    return AUDIO_RET_OK;
}

static uint32_t __get_route_flag(struct userdata *u) {
    uint32_t route_flag = 0;

    if (u->session == AUDIO_SESSION_VOICECALL || u->session == AUDIO_SESSION_VIDEOCALL) {
        if (__is_noise_reduction_on())
            route_flag |= AUDIO_ROUTE_FLAG_NOISE_REDUCTION;
        if (__is_extra_volume_on())
            route_flag |= AUDIO_ROUTE_FLAG_EXTRA_VOL;
        if (__is_wideband())
            route_flag |= AUDIO_ROUTE_FLAG_WB;
    }
    if (u->session == AUDIO_SESSION_VOICE_RECOGNITION && u->subsession_opt & SUBSESSION_OPT_SVOICE) {
        if (u->subsession_opt & SUBSESSION_OPT_COMMAND)
            route_flag |= AUDIO_ROUTE_FLAG_SVOICE_COMMAND;
        else if (u->subsession_opt & SUBSESSION_OPT_WAKEUP)
            route_flag |= AUDIO_ROUTE_FLAG_SVOICE_WAKEUP;
    }

    return route_flag;
}

static audio_return_t policy_set_active_device(struct userdata *u, uint32_t device_in, uint32_t device_out) {
    pa_sink_input *si = NULL;
    uint32_t idx;
    uint32_t route_flag = 0;

    route_flag = __get_route_flag(u);

    pa_log_info("set_active_device session:%s,%s in:%s->%s out:%s->%s flag:%x->%x muteall:%d call_muted:%d",
            __get_session_str(u->session), __get_subsession_str(u->subsession),
            __get_device_in_str(u->active_device_in), __get_device_in_str(device_in),
            __get_device_out_str(u->active_device_out), __get_device_out_str(device_out),
            u->active_route_flag, route_flag, u->muteall, u->call_muted);

    /* Skip duplicated request */
    if ((device_in == AUDIO_DEVICE_IN_NONE || u->active_device_in == device_in) &&
        (device_out == AUDIO_DEVICE_OUT_NONE || u->active_device_out == device_out) &&
        u->active_route_flag == route_flag) {
        pa_log_debug("duplicated request is ignored device_in(%d) device_out(%d) flag(0x%x)", device_in, device_out, route_flag);
        return AUDIO_RET_OK;
    }

    if (u->audio_mgr.set_route) {
        u->audio_mgr.set_route(u->audio_mgr.data, u->session, u->subsession, device_in, device_out, route_flag);
    }

    /* sleep for avoiding sound leak during UCM switching
       this is just a workaround, we should synchronize in future */
    if (device_out != AUDIO_DEVICE_OUT_NONE && u->active_device_out != device_out &&
        u->session != AUDIO_SESSION_VOICECALL && u->session != AUDIO_SESSION_VIDEOCALL) {
        /* Mute sink inputs which are unmuted */
        PA_IDXSET_FOREACH(si, u->core->sink_inputs, idx) {
            if (!pa_sink_input_get_mute(si)) {
                pa_proplist_sets(si->proplist, "module-policy.device_switching", "1");
            }
        }
    }

    /* Update active devices */
    if (device_in != AUDIO_DEVICE_IN_NONE)
        u->active_device_in = device_in;
    if (device_out != AUDIO_DEVICE_OUT_NONE)
        u->active_device_out = device_out;
    u->active_route_flag = route_flag;

    if (u->session == AUDIO_SESSION_VOICECALL) {
        if (u->muteall) {
            policy_set_mute(u, (-1), AUDIO_VOLUME_TYPE_CALL, AUDIO_DIRECTION_OUT, 1);
        }
        /* workaround for keeping call mute setting */
        policy_set_mute(u, (-1), AUDIO_VOLUME_TYPE_CALL, AUDIO_DIRECTION_IN, u->call_muted);
        if (u->audio_mgr.get_volume_level) {
            uint32_t volume_level = 0;

            u->audio_mgr.get_volume_level(u->audio_mgr.data, AUDIO_VOLUME_TYPE_CALL, &volume_level);
            policy_set_volume_level(u, (-1), AUDIO_VOLUME_TYPE_CALL, volume_level);
        }
    }

    return AUDIO_RET_OK;
}

static audio_return_t policy_get_volume_level_max(struct userdata *u, uint32_t volume_type, uint32_t *volume_level) {
    audio_return_t audio_ret = AUDIO_RET_OK;

    /* Call HAL function if exists */
    if (u->audio_mgr.get_volume_level_max) {
        if (AUDIO_IS_ERROR((audio_ret = u->audio_mgr.get_volume_level_max(u->audio_mgr.data, volume_type, volume_level)))) {
            pa_log_error("get_volume_level_max returns error:0x%x", audio_ret);
            return audio_ret;
        }
    }

    pa_log_info("get volume level max type:%d level:%d", volume_type, *volume_level);
    return AUDIO_RET_OK;
}

static audio_return_t __update_volume(struct userdata *u, uint32_t stream_idx, uint32_t volume_type, uint32_t volume_level)
{
    audio_return_t audio_ret = AUDIO_RET_OK;
    pa_sink_input *si = NULL;
    uint32_t idx;
    audio_info_t audio_info;

    /* Update volume as current level if volume_level has -1 */
    if (volume_level == (uint32_t)-1 && stream_idx != (uint32_t)-1) {
        /* Skip updating if stream doesn't have volume type */
        if (policy_get_volume_level(u, stream_idx, &volume_type, &volume_level) == AUDIO_ERR_UNDEFINED) {
            return AUDIO_RET_OK;
        }
    }

    if (u->muteall && (volume_type != AUDIO_VOLUME_TYPE_FIXED)) {
        pa_log_debug("set_mute is called from __update_volume by muteall stream_idx:%d type:%d", stream_idx, volume_type);

        if (policy_set_mute(u, stream_idx, volume_type, AUDIO_DIRECTION_OUT, 1) == AUDIO_RET_USE_HW_CONTROL) {
            return AUDIO_RET_USE_HW_CONTROL;
        };
    }

    /* Call HAL function if exists */
    if (u->audio_mgr.set_volume_level && (stream_idx == PA_INVALID_INDEX)) {
        if (AUDIO_IS_ERROR((audio_ret = u->audio_mgr.set_volume_level(u->audio_mgr.data, NULL, volume_type, volume_level)))) {
            pa_log_error("set_volume_level returns error:0x%x", audio_ret);
            return audio_ret;
        }
    }

    PA_IDXSET_FOREACH(si, u->core->sink_inputs, idx) {

        if (AUDIO_IS_ERROR(__fill_audio_info(si, &audio_info))) {
            /* skip mono sink-input */
            continue;
        }

        /* Update volume of stream if it has requested volume type */
        if ((stream_idx == idx) || ((stream_idx == PA_INVALID_INDEX) && (audio_info.stream.volume_type == volume_type))) {
            double volume_linear = 1.0f;
            pa_cvolume cv;

            /* Call HAL function if exists */
            if (u->audio_mgr.set_volume_level) {
                if (AUDIO_IS_ERROR((audio_ret = u->audio_mgr.set_volume_level(u->audio_mgr.data, &audio_info, audio_info.stream.volume_type, volume_level)))) {
                    pa_log_error("set_volume_level for sink-input[%d] returns error:0x%x", idx, audio_ret);
                    return audio_ret;
                }
            }

            /* Get volume value by type & level */
            if (u->audio_mgr.get_volume_value && (audio_ret != AUDIO_RET_USE_HW_CONTROL)) {
                if (AUDIO_IS_ERROR((audio_ret = u->audio_mgr.get_volume_value(u->audio_mgr.data, &audio_info, audio_info.stream.volume_type, volume_level, &volume_linear)))) {
                    pa_log_warn("get_volume_value for sink-input[%d] returns error:0x%x", idx, audio_ret);
                    return audio_ret;
                }
            }
            pa_cvolume_set(&cv, si->sample_spec.channels, pa_sw_volume_from_linear(volume_linear));

            pa_sink_input_set_volume(si, &cv, TRUE, TRUE);
            if (idx == stream_idx)
                break;
        }
    }

    return audio_ret;
}

int __parse_hdmi_support_info(int hdmi_support, int* channels, int* max_channel){
	int i;
	int bit_mask = 0x00000080;

	*channels = hdmi_support & 0x000000FF;

	if(*channels == 0xFF){
		*channels = 0;
		*max_channel = 0;
		return 0;
	}

	for(i=8; i>=0 ; i--){
		if((bit_mask & *channels)!=0){
			*max_channel = i;
			break;
		}
		bit_mask = bit_mask >> 1;
	}
	return 1;
}

static audio_return_t policy_get_volume_level(struct userdata *u, uint32_t stream_idx, uint32_t *volume_type, uint32_t *volume_level) {
    pa_sink_input *si = NULL;
    const char *si_volume_type_str;

    if (*volume_type == (uint32_t)-1 && stream_idx != (uint32_t)-1) {
        if ((si = pa_idxset_get_by_index(u->core->sink_inputs, stream_idx))) {
            if ((si_volume_type_str = pa_proplist_gets(si->proplist, PA_PROP_MEDIA_TIZEN_VOLUME_TYPE))) {
                pa_atou(si_volume_type_str, volume_type);
            } else {
                pa_log_debug_verbose("stream[%d] doesn't have volume type", stream_idx);
                return AUDIO_ERR_UNDEFINED;
            }
        } else {
            pa_log_warn("stream[%d] doesn't exist", stream_idx);
            return AUDIO_ERR_PARAMETER;
        }
    }

    if( *volume_type >= AUDIO_VOLUME_TYPE_MAX ){
        pa_log_warn("volume_type (%d) invalid", *volume_type);
        return AUDIO_ERR_PARAMETER;
    }
    if (u->audio_mgr.get_volume_level) {
        u->audio_mgr.get_volume_level(u->audio_mgr.data, *volume_type, volume_level);
    }

    pa_log_info("get_volume_level stream_idx:%d type:%d level:%d", stream_idx, *volume_type, *volume_level);
    return AUDIO_RET_OK;
}

static audio_return_t policy_set_volume_level(struct userdata *u, uint32_t stream_idx, uint32_t volume_type, uint32_t volume_level) {

    pa_log_info("set_volume_level stream_idx:%d type:%d level:%d", stream_idx, volume_type, volume_level);

    /* Store volume level of type */
    if (volume_type != (uint32_t)-1) {
        if (u->audio_mgr.set_volume_level) {
            u->audio_mgr.set_volume_level(u->audio_mgr.data, NULL, volume_type, volume_level);
        }
    }

    return __update_volume(u, stream_idx, volume_type, volume_level);
}

static audio_return_t policy_update_volume(struct userdata *u) {
    audio_return_t audio_ret = AUDIO_RET_OK;
    uint32_t volume_type;
    uint32_t volume_level = 0;
    pa_sink_input *si = NULL;
    uint32_t idx;
    audio_info_t audio_info;

    pa_log_info("update_volume");

    for (volume_type = 0; volume_type < AUDIO_VOLUME_TYPE_MAX; volume_type++) {
        if (u->audio_mgr.get_volume_level) {
            u->audio_mgr.get_volume_level(u->audio_mgr.data, volume_type, &volume_level);
        }
        __update_volume(u, (uint32_t)-1, volume_type, volume_level);
    }

    return AUDIO_RET_OK;
}

static audio_return_t policy_get_mute(struct userdata *u, uint32_t stream_idx, uint32_t volume_type, uint32_t direction, uint32_t *mute) {
    audio_return_t audio_ret = AUDIO_RET_OK;
    pa_sink_input *si = NULL;
    uint32_t idx;
    audio_info_t audio_info;

    if (u->audio_mgr.get_mute && (stream_idx == PA_INVALID_INDEX)) {
        audio_ret = u->audio_mgr.get_mute(u->audio_mgr.data, NULL, volume_type, direction, mute);
        if (audio_ret == AUDIO_RET_USE_HW_CONTROL) {
            return audio_ret;
        } else {
            pa_log_error("get_mute returns error:0x%x", audio_ret);
            return audio_ret;
        }
    }

    if (direction == AUDIO_DIRECTION_OUT) {
        PA_IDXSET_FOREACH(si, u->core->sink_inputs, idx) {
            if (AUDIO_IS_ERROR(__fill_audio_info(si, &audio_info))) {
                /* skip mono sink-input */
                continue;
            }

            /* Update mute of stream if it has requested stream or volume type */
            if ((stream_idx == idx) || ((stream_idx == PA_INVALID_INDEX) && (audio_info.stream.volume_type == volume_type))) {

                /* Call HAL function if exists */
                if (u->audio_mgr.get_mute) {
                    audio_ret = u->audio_mgr.get_mute(u->audio_mgr.data, &audio_info, audio_info.stream.volume_type, direction, mute);
                    if (audio_ret == AUDIO_RET_USE_HW_CONTROL) {
                        return audio_ret;
                    } else if (AUDIO_IS_ERROR(audio_ret)) {
                        pa_log_error("get_mute for sink-input[%d] returns error:0x%x", idx, audio_ret);
                        return audio_ret;
                    }
                }

                *mute = (uint32_t)pa_sink_input_get_mute(si);
                break;
            }
        }
    }

    pa_log_info("get mute stream_idx:%d type:%d direction:%d mute:%d", stream_idx, volume_type, direction, *mute);
    return audio_ret;
}

static audio_return_t policy_set_mute(struct userdata *u, uint32_t stream_idx, uint32_t volume_type, uint32_t direction, uint32_t mute) {
    audio_return_t audio_ret = AUDIO_RET_OK;
    pa_sink_input *si = NULL;
    uint32_t idx;
    audio_info_t audio_info;
    const char *si_volume_type_str;

    pa_log_info("set_mute stream_idx:%d type:%d direction:%d mute:%d", stream_idx, volume_type, direction, mute);

    if (volume_type == (uint32_t)-1 && stream_idx != (uint32_t)-1) {
        if ((si = pa_idxset_get_by_index(u->core->sink_inputs, stream_idx))) {
            if ((si_volume_type_str = pa_proplist_gets(si->proplist, PA_PROP_MEDIA_TIZEN_VOLUME_TYPE))) {
                pa_atou(si_volume_type_str, &volume_type);
            } else {
                pa_log_debug_verbose("stream[%d] doesn't have volume type", stream_idx);
                return AUDIO_ERR_UNDEFINED;
            }
        } else {
            pa_log_warn("stream[%d] doesn't exist", stream_idx);
            return AUDIO_ERR_PARAMETER;
        }
    }

    /* workaround for keeping call mute setting */
    if ((volume_type == AUDIO_VOLUME_TYPE_CALL) && (direction == AUDIO_DIRECTION_IN)) {
        u->call_muted = mute;
    }

    if (u->muteall && !mute && (direction == AUDIO_DIRECTION_OUT) && (volume_type != AUDIO_VOLUME_TYPE_FIXED)) {
        pa_log_info("set_mute is ignored by muteall");
        return audio_ret;
    }

    /* Call HAL function if exists */
    if (u->audio_mgr.set_mute && (stream_idx == PA_INVALID_INDEX)) {
        audio_ret = u->audio_mgr.set_mute(u->audio_mgr.data, NULL, volume_type, direction, mute);
        if (audio_ret == AUDIO_RET_USE_HW_CONTROL) {
            pa_log_info("set_mute(call) returns error:0x%x mute:%d", audio_ret, mute);
            return audio_ret;
        } else if (AUDIO_IS_ERROR(audio_ret)) {
            pa_log_error("set_mute returns error:0x%x", audio_ret);
            return audio_ret;
        }
    }

    if (direction == AUDIO_DIRECTION_OUT) {
        PA_IDXSET_FOREACH(si, u->core->sink_inputs, idx) {
            if (AUDIO_IS_ERROR(__fill_audio_info(si, &audio_info))) {
                /* skip mono sink-input */
                continue;
            }

            /* Update mute of stream if it has requested stream or volume type */
            if ((stream_idx == idx) || ((stream_idx == PA_INVALID_INDEX) && (audio_info.stream.volume_type == volume_type))) {

                /* Call HAL function if exists */
                if (u->audio_mgr.set_mute) {
                    audio_ret = u->audio_mgr.set_mute(u->audio_mgr.data, &audio_info, audio_info.stream.volume_type, direction, mute);
                    if (AUDIO_IS_ERROR(audio_ret)) {
                        pa_log_error("set_mute for sink-input[%d] returns error:0x%x", idx, audio_ret);
                        return audio_ret;
                    }
                }

                pa_sink_input_set_mute(si, (pa_bool_t)mute, TRUE);
                if (idx == stream_idx)
                    break;
            }
        }
    }

    return audio_ret;
}

static pa_bool_t policy_is_available_high_latency(struct userdata *u)
{
    pa_sink_input *si = NULL;
    uint32_t idx;
    const char *si_policy_str;

    PA_IDXSET_FOREACH(si, u->core->sink_inputs, idx) {
        if ((si_policy_str = pa_proplist_gets(si->proplist, PA_PROP_MEDIA_POLICY))) {
            if (pa_streq(si_policy_str, POLICY_HIGH_LATENCY) && sink_is_highlatency(si->sink)) {
                pa_log_info("high latency is exists");
                return FALSE;
            }
        }
    }

    return TRUE;
}

static void policy_set_vsp(struct userdata *u, uint32_t stream_idx, uint32_t value)
{
    pa_sink_input *si = NULL;
    audio_info_t audio_info;

    if ((si = pa_idxset_get_by_index(u->core->sink_inputs, stream_idx))) {
        if (AUDIO_IS_ERROR(__fill_audio_info(si, &audio_info))) {
            pa_log_debug("skip set_vsp to sink-input[%d]", stream_idx);
            return;
        }
        if(u->audio_mgr.set_vsp != NULL)
            u->audio_mgr.set_vsp(u->audio_mgr.data, &audio_info, value);
    }

    return;
}

static void policy_set_soundalive_filter_action(struct userdata *u, uint32_t stream_idx, uint32_t value)
{
    pa_sink_input *si = NULL;
    audio_info_t audio_info;

    if ((si = pa_idxset_get_by_index(u->core->sink_inputs, stream_idx))) {
        if (AUDIO_IS_ERROR(__fill_audio_info(si, &audio_info))) {
            pa_log_debug("skip set_soundalive_filter_action to sink-input[%d]", stream_idx);
            return;
        }
        if(u->audio_mgr.set_soundalive_filter_action != NULL)
            u->audio_mgr.set_soundalive_filter_action(u->audio_mgr.data, &audio_info, value);
    }

    return;
}
static void policy_set_soundalive_preset_mode(struct userdata *u, uint32_t stream_idx, uint32_t value)
{
    pa_sink_input *si = NULL;
    audio_info_t audio_info;

    if ((si = pa_idxset_get_by_index(u->core->sink_inputs, stream_idx))) {
        if (AUDIO_IS_ERROR(__fill_audio_info(si, &audio_info))) {
            pa_log_debug("skip set_soundalive_preset_mode to sink-input[%d]", stream_idx);
            return;
        }
        if(u->audio_mgr.set_soundalive_preset_mode != NULL)
            u->audio_mgr.set_soundalive_preset_mode(u->audio_mgr.data, &audio_info, value);
    }

    return;
}

static void policy_set_soundalive_equalizer(struct userdata *u, uint32_t stream_idx, uint32_t* eq)
{
    pa_sink_input *si = NULL;
    audio_info_t audio_info;

    pa_return_if_fail(eq);

    if ((si = pa_idxset_get_by_index(u->core->sink_inputs, stream_idx))) {
        if (AUDIO_IS_ERROR(__fill_audio_info(si, &audio_info))) {
            pa_log_debug("skip set_soundalive_equalizer to sink-input[%d]", stream_idx);
            return;
        }
        if(u->audio_mgr.set_soundalive_equalizer != NULL)
            u->audio_mgr.set_soundalive_equalizer(u->audio_mgr.data, &audio_info, eq);
    }

    return;
}

static void policy_set_soundalive_extend(struct userdata *u, uint32_t stream_idx, uint32_t* ext)
{
    pa_sink_input *si = NULL;
    audio_info_t audio_info;

    pa_return_if_fail(ext);

    if ((si = pa_idxset_get_by_index(u->core->sink_inputs, stream_idx))) {
        if (AUDIO_IS_ERROR(__fill_audio_info(si, &audio_info))) {
            pa_log_debug("skip set_soundalive_extend to sink-input[%d]", stream_idx);
            return;
        }
        if(u->audio_mgr.set_soundalive_extend != NULL)
            u->audio_mgr.set_soundalive_extend(u->audio_mgr.data, &audio_info, ext);
    }

    return;
}

static void policy_set_soundalive_device(struct userdata *u, uint32_t stream_idx, uint32_t value)
{
    pa_sink_input *si = NULL;
    audio_info_t audio_info;

    if ((si = pa_idxset_get_by_index(u->core->sink_inputs, stream_idx))) {
        if (AUDIO_IS_ERROR(__fill_audio_info(si, &audio_info))) {
            pa_log_debug("skip set_soundalive_device to sink-input[%d]", stream_idx);
            return;
        }
        if(u->audio_mgr.set_soundalive_device != NULL)
            u->audio_mgr.set_soundalive_device(u->audio_mgr.data, &audio_info, value);
    }

    return;
}

static void policy_set_dha_param(struct userdata *u, uint32_t stream_idx, uint32_t onoff, uint32_t *gain)
{
    pa_sink_input *si = NULL;
    audio_info_t audio_info;

    pa_return_if_fail(gain);

    if ((si = pa_idxset_get_by_index(u->core->sink_inputs, stream_idx))) {
        if (AUDIO_IS_ERROR(__fill_audio_info(si, &audio_info))) {
            pa_log_debug("skip set_dha_param to sink-input[%d]", stream_idx);
            return;
        }
        if (u->audio_mgr.set_dha_param != NULL)
            u->audio_mgr.set_dha_param(u->audio_mgr.data, &audio_info, onoff, gain);
    }

    return;
}

static void policy_unload_hdmi(struct userdata *u) {
    struct pa_alsa_sink_info *sink_info = NULL;
    struct pa_alsa_sink_info *sink_info_n = NULL;
    pa_sink_input *i = NULL;
    pa_sink *sink_null = NULL,*sink_tmp = NULL;
    int idx = 0;

    PA_LLIST_FOREACH_SAFE(sink_info, sink_info_n, u->alsa_sinks) {
        if (sink_info->name && pa_streq(sink_info->name, SINK_HDMI)) {
            pa_log_info("HDMI sink[%s] found, now unloading this", sink_info->name);
            sink_tmp = (pa_sink *)pa_namereg_get(u->core, sink_info->name, PA_NAMEREG_SINK);
            sink_null = (pa_sink *)pa_namereg_get(u->core, "null", PA_NAMEREG_SINK);
            if(sink_null == NULL || sink_tmp == NULL)
            {
                pa_log_warn("Null sink is not available");
                break;
            }
            pa_log_info("Moving all the sink-input connected to HDMI sink to null-sink");
            PA_IDXSET_FOREACH(i, sink_tmp->inputs, idx) {
                   pa_sink_input_move_to(i, sink_null, TRUE);
            }
            pa_sink_suspend(sink_tmp,TRUE,PA_SUSPEND_APPLICATION);
            PA_LLIST_REMOVE(struct pa_alsa_sink_info, u->alsa_sinks, sink_info);
            pa_module_unload(u->core, sink_info->sink, TRUE);
            if(sink_info->mono_sink)
                pa_module_unload(u->core, sink_info->mono_sink, TRUE);
            pa_xfree(sink_info->pcm_device);
            pa_xfree(sink_info->name);
            pa_xfree(sink_info);
            break;
        }
    }
}

#define EXT_VERSION 1

static int extension_cb(pa_native_protocol *p, pa_module *m, pa_native_connection *c, uint32_t tag, pa_tagstruct *t) {
    struct userdata *u = NULL;
    uint32_t command;
    pa_tagstruct *reply = NULL;

    pa_sink_input *si = NULL;
    pa_sink *s = NULL;
    uint32_t idx;
    pa_sink* sink_to_move = NULL;

    pa_assert(p);
    pa_assert(m);
    pa_assert(c);
    pa_assert(t);

    u = m->userdata;

    if (pa_tagstruct_getu32(t, &command) < 0)
        goto fail;

    reply = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);

    switch (command) {
        case SUBCOMMAND_TEST: {
            if (!pa_tagstruct_eof(t))
                goto fail;

            pa_tagstruct_putu32(reply, EXT_VERSION);
            break;
        }

        case SUBCOMMAND_PLAY_SAMPLE: {
            const char *name;
            uint32_t volume_type = 0;
            uint32_t gain_type = 0;
            uint32_t volume_level = 0;
            uint32_t stream_idx = PA_INVALID_INDEX;

            if (pa_tagstruct_gets(t, &name) < 0 ||
                pa_tagstruct_getu32(t, &volume_type) < 0 ||
                pa_tagstruct_getu32(t, &gain_type) < 0 ||
                pa_tagstruct_getu32(t, &volume_level) < 0 ||
                !pa_tagstruct_eof(t)) {
                pa_log_error("protocol error");
                goto fail;
            }

            policy_play_sample(u, c, name, volume_type, gain_type, volume_level, &stream_idx);

            pa_tagstruct_putu32(reply, stream_idx);
            break;
        }

        case SUBCOMMAND_PLAY_SAMPLE_CONTINUOUSLY: {
            const char *name;
            pa_bool_t start;
            uint32_t volume_type = 0;
            uint32_t gain_type = 0;
            uint32_t volume_level = 0;
            uint32_t stream_idx = PA_INVALID_INDEX;
            pa_usec_t interval;

            if (pa_tagstruct_gets(t, &name) < 0 ||
                pa_tagstruct_get_boolean(t, &start) < 0 ||
                pa_tagstruct_getu32(t, &volume_type) < 0 ||
                pa_tagstruct_getu32(t, &gain_type) < 0 ||
                pa_tagstruct_getu32(t, &volume_level) < 0 ||
                pa_tagstruct_get_usec(t, &interval) < 0 ||
                !pa_tagstruct_eof(t)) {
                pa_log_error("protocol error");
                goto fail;
            }

            if(start == TRUE) {
                pa_log_warn("play_sample_continuously start. name(%s), vol_type(%d), gain_type(%d), vol_level(%d), interval(%lu ms)",
                    name, volume_type, gain_type, volume_level, (unsigned long) (interval / PA_USEC_PER_MSEC));
                policy_play_sample_continuously(u, c, name, interval, volume_type, gain_type, volume_level, &stream_idx);
            } else if(start == FALSE) {
                pa_log_warn("play_sample_continuously end.");
                policy_stop_sample_continuously(u);
            } else {
                pa_log_error("play sample continuously unknown command. name(%s), start(%d)", name, start);
            }

            pa_tagstruct_putu32(reply, stream_idx);
            break;
        }

        case SUBCOMMAND_MONO: {

            pa_bool_t enable;

            if (pa_tagstruct_get_boolean(t, &enable) < 0)
                goto fail;

            pa_log_debug ("new mono value = %d\n", enable);
            if (enable == u->is_mono) {
                pa_log_debug ("No changes in mono value = %d", u->is_mono);
                break;
            }

            u->is_mono = enable;

            /* Move current sink-input to proper mono sink */
            PA_IDXSET_FOREACH(si, u->core->sink_inputs, idx) {
                const char *policy = NULL;

                /* Skip this if it is already in the process of being moved
                 * anyway */
                if (!si->sink)
                    continue;

                /* It might happen that a stream and a sink are set up at the
                   same time, in which case we want to make sure we don't
                   interfere with that */
                if (!PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(si)))
                    continue;

                /* Get role (if role is filter, skip it) */
                if (policy_is_filter(si))
                    continue;

                /* Check policy, if no policy exists, treat as AUTO */
                if (!(policy = pa_proplist_gets(si->proplist, PA_PROP_MEDIA_POLICY))) {
                    pa_log_debug(" set policy of sink-input[%d] from [%s] to [auto]", si->index, "null");
                    policy = POLICY_AUTO;
                }
                pa_log_debug(" Policy of sink input [%d] = %s", si->index, policy);

                /* Select sink to move and move to it */
                sink_to_move = policy_select_proper_sink (u, policy, si, TRUE);
                if (sink_to_move) {
                    pa_log_debug("Moving sink-input[%d] from [%s] to [%s]", si->index, si->sink->name, sink_to_move->name);
                    pa_sink_input_move_to(si, sink_to_move, FALSE);
                } else {
                    pa_log_debug("Can't move sink-input....");
                }
            }
            break;
        }

        case SUBCOMMAND_BALANCE: {
            unsigned i;
            float balance;
            pa_cvolume cvol;
            const pa_cvolume *scvol;
            pa_channel_map map;

            if (pa_tagstruct_get_cvolume(t, &cvol) < 0)
                goto fail;

            pa_channel_map_init_stereo(&map);
            balance = pa_cvolume_get_balance(&cvol, &map);

            pa_log_debug ("new balance value = [%f]\n", balance);

            if (balance == u->balance) {
                pa_log_debug ("No changes in balance value = [%f]", u->balance);
                break;
            }

            u->balance = balance;

            /* Apply balance value to each Sinks */
            PA_IDXSET_FOREACH(s, u->core->sinks, idx) {
                scvol = pa_sink_get_volume(s, FALSE);
                for (i = 0; i < scvol->channels; i++) {
                    cvol.values[i] = scvol->values[i];
                }
                cvol.channels = scvol->channels;

                pa_cvolume_set_balance(&cvol, &s->channel_map, u->balance);
                pa_sink_set_volume(s, &cvol, TRUE, TRUE);
            }
            break;
        }
        case SUBCOMMAND_MUTEALL: {
            pa_bool_t enable;
            unsigned i;
#if 0
            pa_cvolume cvol ;
            pa_cvolume* scvol ;
#endif

            if (pa_tagstruct_get_boolean(t, &enable) < 0)
                goto fail;

            pa_log_debug ("new muteall value = %d\n", enable);
            if (enable == u->muteall) {
                pa_log_debug ("No changes in muteall value = %d", u->muteall);
                break;
            }

            u->muteall = enable;

/* Use mute instead of volume for muteall */
#if 1
            for (i = 0; i < AUDIO_VOLUME_TYPE_MAX; i++) {
                policy_set_mute(u, (-1), i, AUDIO_DIRECTION_OUT, u->muteall);
            }
            PA_IDXSET_FOREACH(si, u->core->sink_inputs, idx) {
                pa_sink_input_set_mute(si, u->muteall, TRUE);
            }
#else
            /* Apply new volume  value to each Sink_input */
            if(u->muteall) {
                PA_IDXSET_FOREACH(si, u->core->sink_inputs, idx) {
                    scvol = pa_sink_input_get_volume (si, &cvol,TRUE);
                    for (i = 0; i < scvol->channels; i++) {
                        scvol->values[i] = 0;
                    }
                    pa_sink_input_set_volume(si,scvol,TRUE,TRUE);
                }
            } else {
                PA_IDXSET_FOREACH(si, u->core->sink_inputs, idx) {
                    if(pa_streq(si->module->name,"module-remap-sink")) {
                        scvol = pa_sink_input_get_volume (si, &cvol,TRUE);
                        for (i = 0; i < scvol->channels; i++) {
                            scvol->values[i] = MAX_VOLUME_FOR_MONO;
                        }
                        pa_sink_input_set_volume(si,scvol,TRUE,TRUE);
                    }
                }
            }
#endif
            break;
        }

#ifdef PA_USE_QCOM_UCM
        case SUBCOMMAND_SET_USE_CASE: {
            const char *verb;
            const char *devices[MAX_DEVICES] = {0};
            const char *modifiers[MAX_MODIFIER] = {0};
            uint32_t num_devices = 0, num_modifiers = 0;
            uint32_t i = 0;

            pa_tagstruct_gets(t, &verb);
            pa_log_debug("verb: %s", verb);
            pa_tagstruct_getu32(t,&num_devices);
            for(i = 0; i < num_devices; i++)
                pa_tagstruct_gets(t, &devices[i]);
            pa_tagstruct_getu32(t,&num_modifiers);
            for(i = 0; i < num_modifiers; i++)
                pa_tagstruct_gets(t, &modifiers[i]);

            pa_uc_mgr_set_use_case(u, verb, devices, modifiers);

            break;
        }
#endif

        case SUBCOMMAND_SET_SESSION: {
            uint32_t session = 0;
            uint32_t start = 0;

            pa_tagstruct_getu32(t, &session);
            pa_tagstruct_getu32(t, &start);

            policy_set_session(u, session, start);
            break;
        }

        case SUBCOMMAND_SET_SUBSESSION: {
            uint32_t subsession = 0;
            uint32_t subsession_opt = 0;

            pa_tagstruct_getu32(t, &subsession);
            pa_tagstruct_getu32(t, &subsession_opt);

            policy_set_subsession(u, subsession, subsession_opt);
            break;
        }

        case SUBCOMMAND_SET_ACTIVE_DEVICE: {
            uint32_t device_in = 0;
            uint32_t device_out = 0;

            pa_tagstruct_getu32(t, &device_in);
            pa_tagstruct_getu32(t, &device_out);

            policy_set_active_device(u, device_in, device_out);
            break;
        }

        case SUBCOMMAND_RESET: {

            policy_reset(u);

            break;
        }

        case SUBCOMMAND_GET_VOLUME_LEVEL_MAX: {
            uint32_t volume_type = 0;
            uint32_t volume_level = 0;

            pa_tagstruct_getu32(t, &volume_type);

            policy_get_volume_level_max(u, volume_type, &volume_level);

            pa_tagstruct_putu32(reply, volume_level);
            break;
        }

        case SUBCOMMAND_GET_VOLUME_LEVEL: {
            uint32_t stream_idx = PA_INVALID_INDEX;
            uint32_t volume_type = 0;
            uint32_t volume_level = 0;

            pa_tagstruct_getu32(t, &stream_idx);
            pa_tagstruct_getu32(t, &volume_type);

            policy_get_volume_level(u, stream_idx, &volume_type, &volume_level);

            pa_tagstruct_putu32(reply, volume_level);
            break;
        }

        case SUBCOMMAND_SET_VOLUME_LEVEL: {
            uint32_t stream_idx = PA_INVALID_INDEX;
            uint32_t volume_type = 0;
            uint32_t volume_level = 0;

            pa_tagstruct_getu32(t, &stream_idx);
            pa_tagstruct_getu32(t, &volume_type);
            pa_tagstruct_getu32(t, &volume_level);

            policy_set_volume_level(u, stream_idx, volume_type, volume_level);
            break;
        }

        case SUBCOMMAND_UPDATE_VOLUME: {
            policy_update_volume(u);
            break;
        }

        case SUBCOMMAND_GET_MUTE: {
            uint32_t stream_idx = PA_INVALID_INDEX;
            uint32_t volume_type = 0;
            uint32_t direction = 0;
            uint32_t mute = 0;

            pa_tagstruct_getu32(t, &stream_idx);
            pa_tagstruct_getu32(t, &volume_type);
            pa_tagstruct_getu32(t, &direction);

            policy_get_mute(u, stream_idx, volume_type, direction, &mute);

            pa_tagstruct_putu32(reply, mute);
            break;
        }

        case SUBCOMMAND_SET_MUTE: {
            uint32_t stream_idx = PA_INVALID_INDEX;
            uint32_t volume_type = 0;
            uint32_t direction = 0;
            uint32_t mute = 0;

            pa_tagstruct_getu32(t, &stream_idx);
            pa_tagstruct_getu32(t, &volume_type);
            pa_tagstruct_getu32(t, &direction);
            pa_tagstruct_getu32(t, &mute);

            policy_set_mute(u, stream_idx, volume_type, direction, mute);
            break;
        }
        case SUBCOMMAND_IS_AVAILABLE_HIGH_LATENCY: {
            pa_bool_t available = FALSE;

            available = policy_is_available_high_latency(u);

            pa_tagstruct_putu32(reply, (uint32_t)available);
            break;
        }
        case SUBCOMMAND_VSP_SPEED : {
            uint32_t stream_idx = PA_INVALID_INDEX;
            uint32_t value;

            if (pa_tagstruct_getu32(t, &stream_idx) < 0)
                goto fail;
            if (pa_tagstruct_getu32(t, &value) < 0)
                goto fail;

            policy_set_vsp(u, stream_idx, value);
            break;
        }

        case SUBCOMMAND_SA_FILTER_ACTION: {
            uint32_t stream_idx = PA_INVALID_INDEX;
            uint32_t value;

            if (pa_tagstruct_getu32(t, &stream_idx) < 0)
                goto fail;
            if (pa_tagstruct_getu32(t, &value) < 0)
                goto fail;

            policy_set_soundalive_filter_action(u, stream_idx, value);
            break;
        }
        case SUBCOMMAND_SA_PRESET_MODE: {
            uint32_t stream_idx = PA_INVALID_INDEX;
            uint32_t value;

            if (pa_tagstruct_getu32(t, &stream_idx) < 0)
                goto fail;
            if (pa_tagstruct_getu32(t, &value) < 0)
                goto fail;

            policy_set_soundalive_preset_mode(u, stream_idx, value);
            break;
        }
        case SUBCOMMAND_SA_EQ: {
            uint32_t stream_idx = PA_INVALID_INDEX;
            int i = 0;
            uint32_t eq[EQ_USER_SLOT_NUM];

            if (pa_tagstruct_getu32(t, &stream_idx) < 0)
                goto fail;
            for (i = 0; i < EQ_USER_SLOT_NUM; i++) {
                if (pa_tagstruct_getu32(t, &eq[i]) < 0)
                    goto fail;
            }
            policy_set_soundalive_equalizer(u, stream_idx, eq);
            break;
        }
        case SUBCOMMAND_SA_EXTEND: {
            uint32_t stream_idx = PA_INVALID_INDEX;
            int i = 0;
            uint32_t ext[CUSTOM_EXT_PARAM_MAX];

            if (pa_tagstruct_getu32(t, &stream_idx) < 0)
                goto fail;
            for (i = 0; i < CUSTOM_EXT_PARAM_MAX; i++) {
                if (pa_tagstruct_getu32(t, &ext[i]) < 0)
                    goto fail;
            }

            policy_set_soundalive_extend(u, stream_idx, ext);
            break;
        }
        case SUBCOMMAND_SA_DEVICE: {
            uint32_t stream_idx = PA_INVALID_INDEX;
            uint32_t value;

            if (pa_tagstruct_getu32(t, &stream_idx) < 0)
                goto fail;
            if (pa_tagstruct_getu32(t, &value) < 0)
                goto fail;

            policy_set_soundalive_device(u, stream_idx, value);
            break;
        }

        case SUBCOMMAND_DHA_PARAM: {
            uint32_t stream_idx = PA_INVALID_INDEX;
            uint32_t onoff = 0;
            int i = 0;
            uint32_t gain[DHA_GAIN_NUM];

            if (pa_tagstruct_getu32(t, &stream_idx) < 0)
                goto fail;
            if (pa_tagstruct_getu32(t, &onoff) < 0)
                goto fail;
            for (i = 0; i < DHA_GAIN_NUM; i++) {
                if (pa_tagstruct_getu32(t, &gain[i]) < 0)
                    goto fail;
            }
            policy_set_dha_param(u, stream_idx, onoff, gain);
            break;
        }

        case SUBCOMMAND_UNLOAD_HDMI: {
            policy_unload_hdmi(u);
            break;
        }

        default:
            goto fail;
    }

    pa_pstream_send_tagstruct(pa_native_connection_get_pstream(c), reply);
    return 0;

    fail:

    if (reply)
        pa_tagstruct_free(reply);

    return -1;
}

/*  Called when new sink-input is creating  */
static pa_hook_result_t sink_input_new_hook_callback(pa_core *c, pa_sink_input_new_data *new_data, struct userdata *u)
{
    audio_return_t audio_ret = AUDIO_RET_OK;
    audio_info_t audio_info;
    const char *policy;
    const char *master_name;
    pa_sink *realsink = NULL;
    uint32_t volume_level = 0;
    pa_strbuf *s = NULL;

    pa_assert(c);
    pa_assert(new_data);
    pa_assert(u);

    if (!new_data->proplist) {
        pa_log_debug(" New stream lacks property data.");
        return PA_HOOK_OK;
    }

    /* If sink-input has already sink, skip */
    if (new_data->sink) {
        /* sink-input with filter role will be also here because sink is already set */
#ifdef DEBUG_DETAIL
        pa_log_debug(" Not setting device for stream [%s], because already set.",
                pa_strnull(pa_proplist_gets(new_data->proplist, PA_PROP_MEDIA_NAME)));
#endif
        return PA_HOOK_OK;
    }

    /* If no policy exists, skip */
    if (!(policy = pa_proplist_gets(new_data->proplist, PA_PROP_MEDIA_POLICY))) {
        pa_log_debug("Not setting device for stream [%s], because it lacks policy.",
                pa_strnull(pa_proplist_gets(new_data->proplist, PA_PROP_MEDIA_NAME)));
        return PA_HOOK_OK;
    }

    /* Set proper sink to sink-input */
    new_data->save_sink = FALSE;
    new_data->sink = policy_select_proper_sink (u, policy, NULL, TRUE);

    if (new_data->sink == NULL) {
        pa_log_error("new_data->sink is null");
        goto exit;
    }

    s = pa_strbuf_new();
    master_name = pa_proplist_gets(new_data->sink->proplist, PA_PROP_DEVICE_MASTER_DEVICE);
    if (master_name)
        realsink = pa_namereg_get(c, master_name, PA_NAMEREG_SINK);

    if (AUDIO_IS_ERROR((audio_ret = __fill_audio_stream_info(new_data->proplist, &new_data->sample_spec, &audio_info)))) {
        pa_log_debug("__fill_audio_stream_info returns 0x%x", audio_ret);
    } else if (AUDIO_IS_ERROR((audio_ret = __fill_audio_device_info(realsink? realsink->proplist : new_data->sink->proplist, &audio_info)))) {
        pa_log_debug("__fill_audio_device_info returns 0x%x", audio_ret);
    } else {
        double volume_linear = 1.0f;

        if (u->audio_mgr.get_volume_level) {
            u->audio_mgr.get_volume_level(u->audio_mgr.data, audio_info.stream.volume_type, &volume_level);
        }

        pa_strbuf_printf(s, "[%s] policy[%s] ch[%d] rate[%d] volume&gain[%d,%d] level[%d]",
                audio_info.stream.name, policy, audio_info.stream.channels, audio_info.stream.samplerate,
                audio_info.stream.volume_type, audio_info.stream.gain_type, volume_level);

        if (audio_info.device.api == AUDIO_DEVICE_API_ALSA) {
            pa_strbuf_printf(s, " device:ALSA[%d,%d]", audio_info.device.alsa.card_idx, audio_info.device.alsa.device_idx);
        } else if (audio_info.device.api == AUDIO_DEVICE_API_BLUEZ) {
            pa_strbuf_printf(s, " device:BLUEZ[%s] nrec[%d]", audio_info.device.bluez.protocol, audio_info.device.bluez.nrec);
        }
        pa_strbuf_printf(s, " sink[%s]", (new_data->sink)? new_data->sink->name : "null");

        /* Call HAL function if exists */
        if (u->audio_mgr.set_volume_level) {
            if (AUDIO_IS_ERROR((audio_ret = u->audio_mgr.set_volume_level(u->audio_mgr.data, &audio_info, audio_info.stream.volume_type, volume_level)))) {
                pa_log_warn("set_volume_level for new sink-input returns error:0x%x", audio_ret);
                goto exit;
            }
        }

        /* Get volume value by type & level */
        if (u->audio_mgr.get_volume_value && (audio_ret != AUDIO_RET_USE_HW_CONTROL)) {
            if (AUDIO_IS_ERROR((audio_ret = u->audio_mgr.get_volume_value(u->audio_mgr.data, &audio_info, audio_info.stream.volume_type, volume_level, &volume_linear)))) {
                pa_log_warn("get_volume_value for new sink-input returns error:0x%x", audio_ret);
                goto exit;
            }
        }

        pa_cvolume_init(&new_data->volume);
        pa_cvolume_set(&new_data->volume, new_data->sample_spec.channels, pa_sw_volume_from_linear(volume_linear));
        new_data->volume_is_set = TRUE;
    }

exit:
    if (s) {
        pa_log_info("new %s", pa_strbuf_tostring_free(s));
    }

    return PA_HOOK_OK;
}

/*  Called when new sink is added while sink-input is existing  */
static pa_hook_result_t sink_put_hook_callback(pa_core *c, pa_sink *sink, struct userdata *u)
{
    pa_sink_input *si;
    pa_sink *sink_to_move;
    uint32_t idx;
    unsigned i;
    char *args = NULL;
    pa_cvolume cvol;
    const pa_cvolume *scvol;
    pa_bool_t is_bt;
    pa_bool_t is_usb_alsa;
    pa_bool_t is_need_to_move = true;
    int dock_status;
    uint32_t device_out = AUDIO_DEVICE_OUT_BT_A2DP;

    pa_assert(c);
    pa_assert(sink);
    pa_assert(u);
    pa_assert(u->on_hotplug);

    /* If connected sink is BLUETOOTH, set as default */
    /* we are checking with device.api property */
    is_bt = policy_is_bluez(sink);
    is_usb_alsa = policy_is_usb_alsa(sink);

    if (is_bt || is_usb_alsa) {
        if (u->session == AUDIO_SESSION_VOICECALL || u->session == AUDIO_SESSION_VIDEOCALL || u->session == AUDIO_SESSION_VOIP) {
            pa_log_info("current session is communication mode [%d], no need to move", u->session);
            is_need_to_move = false;
        } else if (is_usb_alsa) {
            vconf_get_int(VCONFKEY_SYSMAN_CRADLE_STATUS, &dock_status);
            if ((dock_status == DOCK_DESKDOCK) || (dock_status == DOCK_CARDOCK)) {
                device_out = AUDIO_DEVICE_OUT_DOCK;
            } else if (dock_status == DOCK_AUDIODOCK) {
                device_out = AUDIO_DEVICE_OUT_MULTIMEDIA_DOCK;
            } else if (dock_status == DOCK_SMARTDOCK) {
                is_need_to_move = false;
            } else {
                device_out = AUDIO_DEVICE_OUT_USB_AUDIO;
                pa_log_info ("This device might be general USB Headset");
            }
        }
    } else {
        pa_log_debug("this sink [%s][%d] is not a bluez....return", sink->name, sink->index);
        return PA_HOOK_OK;
    }

    if (is_bt) {
        /* Load mono_bt sink */
        args = pa_sprintf_malloc("sink_name=%s master=%s channels=1", SINK_MONO_BT, sink->name);
        u->module_mono_bt = pa_module_load(u->module->core, "module-remap-sink", args);
        pa_xfree(args);

        device_out = AUDIO_DEVICE_OUT_BT_A2DP;
    }

    if (is_need_to_move) {
        pa_log_info("set default sink to sink[%s][%d]", sink->name, sink->index);
        pa_namereg_set_default_sink (c,sink);

        /* Set active device out */
        if (u->active_device_out != device_out) {
            uint32_t route_flag = 0;

            route_flag = __get_route_flag(u);

            if (u->audio_mgr.set_route) {
                u->audio_mgr.set_route(u->audio_mgr.data, u->session, u->subsession, u->active_device_in, device_out, route_flag);
            }

            u->active_device_out = device_out;
            u->active_route_flag = route_flag;
        }

        /* Iterate each sink inputs to decide whether we should move to new sink */
        PA_IDXSET_FOREACH(si, c->sink_inputs, idx) {
            const char *policy = NULL;

            if (si->sink == sink)
                continue;

            /* Skip this if it is already in the process of being moved
                    * anyway */
            if (!si->sink)
                continue;

            /* It might happen that a stream and a sink are set up at the
                    same time, in which case we want to make sure we don't
                    interfere with that */
            if (!PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(si)))
                continue;

            /* Get role (if role is filter, skip it) */
            if (policy_is_filter(si))
                continue;

            /* Check policy */
            if (!(policy = pa_proplist_gets(si->proplist, PA_PROP_MEDIA_POLICY))) {
                /* No policy exists, this means auto */
                pa_log_debug("set policy of sink-input[%d] from [%s] to [auto]", si->index, "null");
                policy = POLICY_AUTO;
            }

            sink_to_move = policy_select_proper_sink (u, policy, si, TRUE);
            if (sink_to_move) {
                pa_log_debug("Moving sink-input[%d] from [%s] to [%s]", si->index, si->sink->name, sink_to_move->name);
                pa_sink_input_move_to(si, sink_to_move, FALSE);
            } else {
                pa_log_debug("Can't move sink-input....");
            }
        }
    }

    /* Reset sink volume with balance from userdata */
    scvol = pa_sink_get_volume(sink, FALSE);
    for (i = 0; i < scvol->channels; i++) {
        cvol.values[i] = scvol->values[i];
    }
    cvol.channels = scvol->channels;

    pa_cvolume_set_balance(&cvol, &sink->channel_map, u->balance);
    pa_sink_set_volume(sink, &cvol, TRUE, TRUE);

    /* Reset sink muteall from userdata */
//    pa_sink_set_mute(sink,u->muteall,TRUE);

    return PA_HOOK_OK;
}

static void subscribe_cb(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata)
{
    struct userdata *u = userdata;
    pa_sink *def;
    pa_sink_input *si;
    uint32_t idx2;
    pa_sink *sink_to_move = NULL;
    pa_sink *sink_cur = NULL;
    pa_source *source_cur = NULL;
    pa_source_state_t source_state;
    int vconf_source_status = 0;
    uint32_t si_index;
    int audio_ret;

    pa_assert(u);

    pa_log_debug("t=[0x%x], idx=[%d]", t, idx);

    if (t == (PA_SUBSCRIPTION_EVENT_SERVER|PA_SUBSCRIPTION_EVENT_CHANGE)) {

        def = pa_namereg_get_default_sink(c);
        if (def == NULL) {
            pa_log_warn("pa_namereg_get_default_sink() returns null");
            return;
        }
        pa_log_info("default sink is now [%s]", def->name);

        /* Iterate each sink inputs to decide whether we should move to new DEFAULT sink */
        PA_IDXSET_FOREACH(si, c->sink_inputs, idx2) {
            const char *policy = NULL;

            if (!si->sink)
                continue;

            /* Get role (if role is filter, skip it) */
            if (policy_is_filter(si))
                continue;

            if (pa_streq (def->name, "null")) {
                pa_log_debug("Moving sink-input[%d] from [%s] to [%s]", si->index, si->sink->name, def->name);
                pa_sink_input_move_to(si, def, FALSE);
                continue;
            }

            /* Get policy */
            if (!(policy = pa_proplist_gets(si->proplist, PA_PROP_MEDIA_POLICY))) {
                /* No policy exists, this means auto */
                pa_log_debug("set policy of sink-input[%d] from [%s] to [auto]", si->index, "null");
                policy = POLICY_AUTO;
            }

            sink_to_move = policy_select_proper_sink (u, policy, si, TRUE);
            if (sink_to_move) {
                /* Move sink-input to new DEFAULT sink */
                pa_log_debug("Moving sink-input[%d] from [%s] to [%s]", si->index, si->sink->name, sink_to_move->name);
                pa_sink_input_move_to(si, sink_to_move, FALSE);
            }
        }
        pa_log_info("All moved to proper sink finished!!!!");
    } else if (t == (PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE)) {
        if ((sink_cur = pa_idxset_get_by_index(c->sinks, idx))) {
            pa_sink_state_t state = pa_sink_get_state(sink_cur);
            pa_log_debug ("@@ sink name = %s, state = %d", sink_cur->name, state);

            if (pa_streq (sink_cur->name, SINK_HIGH_LATENCY) && state == PA_SINK_RUNNING) {
                PA_IDXSET_FOREACH(si, c->sink_inputs, si_index) {
                    if (!si->sink)
                        continue;

                    if (pa_streq (si->sink->name, SINK_HIGH_LATENCY) || pa_streq (si->sink->name, SINK_MONO_HIGH_LATENCY)) {
                        if (AUDIO_IS_ERROR((audio_ret = __update_volume(u, si->index, (uint32_t)-1, (uint32_t)-1)))) {
                            pa_log_debug("__update_volume for stream[%d] returns error:0x%x", si->index, audio_ret);
                        }
                    }
                }
            }
        }
    } else if (t == (PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE)) {
        if ((source_cur = pa_idxset_get_by_index(c->sources, idx))) {
            if (pa_streq (source_cur->name, SOURCE_ALSA)) {
                source_state = pa_source_get_state(source_cur);
                pa_log_debug ("@@ source name = %s, state = %d", source_cur->name, source_state);
                if (source_state == PA_SOURCE_RUNNING) {
                    vconf_set_int (VCONFKEY_SOUND_CAPTURE_STATUS, 1);
                } else {
                    vconf_get_int (VCONFKEY_SOUND_CAPTURE_STATUS, &vconf_source_status);
                    if (vconf_source_status)
                        vconf_set_int (VCONFKEY_SOUND_CAPTURE_STATUS, 0);
                }
            }
        }
    }
}

static pa_hook_result_t sink_unlink_hook_callback(pa_core *c, pa_sink *sink, void* userdata) {
    struct userdata *u = userdata;
    uint32_t idx;
    pa_sink *sink_to_move;
    pa_sink_input *si;

    pa_assert(c);
    pa_assert(sink);
    pa_assert(u);

     /* There's no point in doing anything if the core is shut down anyway */
    if (c->state == PA_CORE_SHUTDOWN)
        return PA_HOOK_OK;

    /* if unloading sink is not bt, just return */
    if (!policy_is_bluez (sink)) {
        pa_log_debug("sink[%s][%d] unlinked but not a bluez....return\n", sink->name, sink->index);
        return PA_HOOK_OK;
    }

    pa_log_debug ("========= sink [%s][%d], bt_off_idx was [%d], now set to [%d]", sink->name, sink->index,u->bt_off_idx, sink->index);
    u->bt_off_idx = sink->index;

    sink_to_move = pa_namereg_get(c, "null", PA_NAMEREG_SINK);

    /* BT sink is unloading, move sink-input to proper sink */
    PA_IDXSET_FOREACH(si, c->sink_inputs, idx) {
        const char *policy = NULL;

        if (!si->sink)
            continue;

        /* Get role (if role is filter, skip it) */
        if (policy_is_filter(si))
            continue;

        /* Find who were using bt sink or bt related sink and move them to proper sink (alsa/mono_alsa) */
        if (pa_streq (si->sink->name, SINK_MONO_BT) ||
            pa_streq (si->sink->name, SINK_MONO_COMBINED) ||
            pa_streq (si->sink->name, SINK_COMBINED) ||
            policy_is_bluez (si->sink)) {

            pa_log_info("[%d] Moving sink-input[%d][%s] from [%s] to [%s]", idx, si->index, policy, si->sink->name, sink_to_move->name);
            pa_sink_input_move_to(si, sink_to_move, FALSE);
        }
    }

    pa_log_debug ("unload sink in dependencies");

    /* Unload mono_combine sink */
    if (u->module_mono_combined) {
        pa_module_unload(u->module->core, u->module_mono_combined, TRUE);
        u->module_mono_combined = NULL;
    }

    /* Unload combine sink */
    if (u->module_combined) {
        pa_module_unload(u->module->core, u->module_combined, TRUE);
        u->module_combined = NULL;
    }

    /* Unload mono_bt sink */
    if (u->module_mono_bt) {
        pa_module_unload(u->module->core, u->module_mono_bt, TRUE);
        u->module_mono_bt = NULL;
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_unlink_post_hook_callback(pa_core *c, pa_sink *sink, void* userdata) {
    struct userdata *u = userdata;

    pa_assert(c);
    pa_assert(sink);
    pa_assert(u);

    pa_log_debug("========= sink [%s][%d]", sink->name, sink->index);

     /* There's no point in doing anything if the core is shut down anyway */
    if (c->state == PA_CORE_SHUTDOWN)
        return PA_HOOK_OK;

    /* if unloading sink is not bt, just return */
    if (!policy_is_bluez (sink)) {
        pa_log_debug("not a bluez....return\n");
        return PA_HOOK_OK;
    }

    u->bt_off_idx = -1;
    pa_log_debug ("bt_off_idx is cleared to [%d]", u->bt_off_idx);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_move_start_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    audio_return_t audio_ret = AUDIO_RET_OK;

    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    /* There's no point in doing anything if the core is shut down anyway */
   if (core->state == PA_CORE_SHUTDOWN)
       return PA_HOOK_OK;


    pa_log_debug ("------- sink-input [%d] was sink [%s][%d] : Trying to mute!!!",
            i->index, i->sink->name, i->sink->index);

    if (AUDIO_IS_ERROR((audio_ret = policy_set_mute(u, i->index, (uint32_t)-1, AUDIO_DIRECTION_OUT, 1)))) {
        pa_log_warn("policy_set_mute(1) for stream[%d] returns error:0x%x", i->index, audio_ret);
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_move_finish_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    audio_return_t audio_ret = AUDIO_RET_OK;

    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    /* There's no point in doing anything if the core is shut down anyway */
    if (core->state == PA_CORE_SHUTDOWN)
        return PA_HOOK_OK;

    pa_log_debug("------- sink-input [%d], sink [%s][%d], bt_off_idx [%d] : %s",
            i->index, i->sink->name, i->sink->index, u->bt_off_idx,
            (u->bt_off_idx == -1)? "Trying to un-mute!!!!" : "skip un-mute...");

    /* If sink input move is caused by bt sink unlink, then skip un-mute operation */
    if (u->bt_off_idx == -1 && !u->muteall) {
        if (AUDIO_IS_ERROR((audio_ret = __update_volume(u, i->index, (uint32_t)-1, (uint32_t)-1)))) {
            pa_log_debug("__update_volume for stream[%d] returns error:0x%x", i->index, audio_ret);
        }
        if (AUDIO_IS_ERROR((audio_ret = policy_set_mute(u, i->index, (uint32_t)-1, AUDIO_DIRECTION_OUT, 0)))) {
            pa_log_debug("policy_set_mute(0) for stream[%d] returns error:0x%x", i->index, audio_ret);
        }
    }

    return PA_HOOK_OK;
}

static pa_source* policy_get_source_by_name (pa_core *c, const char* source_name)
{
    return (pa_source*)pa_namereg_get(c, source_name, PA_NAMEREG_SOURCE);
}

/* Select source for given condition */
static pa_source* policy_select_proper_source (struct userdata *u, const char* policy)
{
    pa_core *c = NULL;
    pa_source* source = NULL;
    pa_source* def = NULL;

    if (policy == NULL) {
        pa_log_warn ("input param is null");
        return NULL;
    }

    pa_assert (u);
    c = u->core;
    pa_assert (c);

    def = pa_namereg_get_default_source(c);
    if (def == NULL) {
        pa_log_warn ("pa_namereg_get_default_source() returns null");
        return NULL;
    }


    /* Select source  to */
    if (pa_streq(policy, POLICY_VOIP)) {
        /* NOTE: Check voip source first, if not available, use AEC source  */
        source = policy_get_source_by_name (c, SOURCE_VOIP);
        if (source == NULL) {
            pa_log_info ("VOIP source is not available, try to use AEC source");
            source = policy_get_source_by_name (c, AEC_SOURCE);
            if (source == NULL) {
                pa_log_warn ("AEC source is not available, set to default source");
                source = def;
            }
        }
    } else if (pa_streq(policy, POLICY_MIRRORING)) {
        source = policy_get_source_by_name (c, SOURCE_MIRRORING);
        if (source == NULL) {
            pa_log_info ("MIRRORING source is not available, try to use ALSA MONITOR SOURCE");
            source = policy_get_source_by_name (c, ALSA_MONITOR_SOURCE);
            if (source == NULL) {
                pa_log_warn (" ALSA MONITOR SOURCE source is not available, set to default source");
                source = def;
            }
        }
    } else {
        if (u->subsession == AUDIO_SUBSESSION_RINGTONE)
            source = policy_get_source_by_name(c, AEC_SOURCE);
        if (!source)
            source = def;
    }

    pa_log_debug ("selected source : [%s]\n", (source)? source->name : "null");
    return source;
}


/*  Called when new source-output is creating  */
static pa_hook_result_t source_output_new_hook_callback(pa_core *c, pa_source_output_new_data *new_data, struct userdata *u) {
    const char *policy = NULL;
    pa_assert(c);
    pa_assert(new_data);
    pa_assert(u);

    if (!new_data->proplist) {
        pa_log_debug("New stream lacks property data.");
        return PA_HOOK_OK;
    }

    if (new_data->source) {
        pa_log_debug("Not setting device for stream %s, because already set.", pa_strnull(pa_proplist_gets(new_data->proplist, PA_PROP_MEDIA_NAME)));
        return PA_HOOK_OK;
    }

    /* If no policy exists, skip */
    if (!(policy = pa_proplist_gets(new_data->proplist, PA_PROP_MEDIA_POLICY))) {
        pa_log_debug("Not setting device for stream [%s], because it lacks policy.",
                pa_strnull(pa_proplist_gets(new_data->proplist, PA_PROP_MEDIA_NAME)));
        return PA_HOOK_OK;
    }
    pa_log_debug("Policy for stream [%s] = [%s]",
            pa_strnull(pa_proplist_gets(new_data->proplist, PA_PROP_MEDIA_NAME)), policy);

    /* Set proper source to source-output */
    new_data->save_source= FALSE;
    new_data->source= policy_select_proper_source (u, policy);

    pa_log_debug("set source of source-input to [%s]", (new_data->source)? new_data->source->name : "null");

    return PA_HOOK_OK;
}


int pa__init(pa_module *m)
{
    pa_modargs *ma = NULL;
    struct userdata *u;
    pa_bool_t on_hotplug = TRUE, on_rescue = TRUE, wideband = FALSE;
    uint32_t frag_size = 0, tsched_size = 0;
    int ret = 0;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "on_hotplug", &on_hotplug) < 0 ||
        pa_modargs_get_value_boolean(ma, "on_rescue", &on_rescue) < 0) {
        pa_log("on_hotplug= and on_rescue= expect boolean arguments");
        goto fail;
    }

        if(pa_modargs_get_value_boolean(ma, "use_wideband_voice", &wideband) < 0 ||
        pa_modargs_get_value_u32(ma, "fragment_size", &frag_size) < 0 ||
        pa_modargs_get_value_u32(ma, "tsched_buffer_size", &tsched_size) < 0 ) {
        pa_log("Failed to parse module arguments buffer info");
        goto fail;
    }
    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->on_hotplug = on_hotplug;
    u->snd_playback_pcm = NULL;
    u->snd_capture_pcm = NULL;
    u->wideband = wideband;
    u->fragment_size = frag_size;
    u->tsched_buffer_size = tsched_size;

    ret = pa_uc_mgr_init(u);
    if(ret < 0) {
        pa_log_info("pa_ucmgr_init failed!");
    } else {
        PA_LLIST_HEAD_INIT(struct pa_alsa_sink_info, u->alsa_sinks);
        PA_LLIST_HEAD_INIT(struct pa_alsa_source_info, u->alsa_sources);
        pa_log_debug("ucm init done");
    }

    /* A little bit later than module-stream-restore */
    u->sink_input_new_hook_slot =
            pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_NEW], PA_HOOK_EARLY+10, (pa_hook_cb_t) sink_input_new_hook_callback, u);

    u->source_output_new_hook_slot =
            pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_NEW], PA_HOOK_EARLY+10, (pa_hook_cb_t) source_output_new_hook_callback, u);

    if (on_hotplug) {
        /* A little bit later than module-stream-restore */
        u->sink_put_hook_slot =
            pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_PUT], PA_HOOK_LATE+10, (pa_hook_cb_t) sink_put_hook_callback, u);
    }

    /* sink unlink comes before sink-input unlink */
    u->sink_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_UNLINK], PA_HOOK_EARLY, (pa_hook_cb_t) sink_unlink_hook_callback, u);
    u->sink_unlink_post_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_UNLINK_POST], PA_HOOK_EARLY, (pa_hook_cb_t) sink_unlink_post_hook_callback, u);

    u->sink_input_move_start_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_START], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_move_start_cb, u);
    u->sink_input_move_finish_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FINISH], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_move_finish_cb, u);

    u->subscription = pa_subscription_new(u->core, PA_SUBSCRIPTION_MASK_SERVER | PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE, subscribe_cb, u);

    pa_log_debug("subscription done");

    u->bt_off_idx = -1;    /* initial bt off sink index */

    u->module_mono_bt = NULL;
    u->module_combined = NULL;
    u->module_mono_combined = NULL;

    u->protocol = pa_native_protocol_get(m->core);
    pa_native_protocol_install_ext(u->protocol, m, extension_cb);

    /* Get mono key value for init */
#ifndef PA_DISABLE_MONO_AUDIO
    vconf_get_bool(MONO_KEY, &u->is_mono);
#else
    u->is_mono = 0;
#endif

    /* Load library & init audio mgr */
    u->audio_mgr.dl_handle = dlopen(LIB_TIZEN_AUDIO, RTLD_NOW);
    if (u->audio_mgr.dl_handle) {
        u->audio_mgr.init = dlsym(u->audio_mgr.dl_handle, "audio_init");
        u->audio_mgr.deinit = dlsym(u->audio_mgr.dl_handle, "audio_deinit");
        u->audio_mgr.reset = dlsym(u->audio_mgr.dl_handle, "audio_reset");
        u->audio_mgr.get_volume_level_max = dlsym(u->audio_mgr.dl_handle, "audio_get_volume_level_max");
        u->audio_mgr.get_volume_level = dlsym(u->audio_mgr.dl_handle, "audio_get_volume_level");
        u->audio_mgr.get_volume_value = dlsym(u->audio_mgr.dl_handle, "audio_get_volume_value");
        u->audio_mgr.set_volume_level = dlsym(u->audio_mgr.dl_handle, "audio_set_volume_level");
        u->audio_mgr.get_mute = dlsym(u->audio_mgr.dl_handle, "audio_get_mute");
        u->audio_mgr.set_mute = dlsym(u->audio_mgr.dl_handle, "audio_set_mute");
        u->audio_mgr.set_session = dlsym(u->audio_mgr.dl_handle, "audio_set_session");
        u->audio_mgr.set_route = dlsym(u->audio_mgr.dl_handle, "audio_set_route");
        u->audio_mgr.set_ecrx_device = dlsym(u->audio_mgr.dl_handle, "audio_set_ecrx_device");
        u->audio_mgr.set_vsp = dlsym(u->audio_mgr.dl_handle, "audio_set_vsp");
        u->audio_mgr.set_soundalive_device = dlsym(u->audio_mgr.dl_handle, "audio_set_soundalive_device");
        u->audio_mgr.set_soundalive_filter_action = dlsym(u->audio_mgr.dl_handle, "audio_set_soundalive_filter_action");
        u->audio_mgr.set_soundalive_preset_mode = dlsym(u->audio_mgr.dl_handle, "audio_set_soundalive_preset_mode");
        u->audio_mgr.set_soundalive_equalizer = dlsym(u->audio_mgr.dl_handle, "audio_set_soundalive_equalizer");
        u->audio_mgr.set_soundalive_extend = dlsym(u->audio_mgr.dl_handle, "audio_set_soundalive_extend");
        u->audio_mgr.set_dha_param = dlsym(u->audio_mgr.dl_handle, "audio_set_dha_param");

        if (u->audio_mgr.init) {
            if (u->audio_mgr.init(&u->audio_mgr.data) != AUDIO_RET_OK) {
                pa_log_error("audio_mgr init failed");
            }
        }
    } else {
        pa_log_error("open audio_mgr failed :%s", dlerror());
    }

    __load_dump_config(u);

    pa_log_info("policy module is loaded\n");

    if (ma)
        pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

void pa__done(pa_module *m)
{
    struct userdata* u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink_input_new_hook_slot)
        pa_hook_slot_free(u->sink_input_new_hook_slot);
    if (u->sink_put_hook_slot)
        pa_hook_slot_free(u->sink_put_hook_slot);
    if (u->subscription)
        pa_subscription_free(u->subscription);
    if (u->protocol) {
        pa_native_protocol_remove_ext(u->protocol, m);
        pa_native_protocol_unref(u->protocol);
    }
    if (u->source_output_new_hook_slot)
        pa_hook_slot_free(u->source_output_new_hook_slot);

    pa_uc_mgr_deinit(u);

    /* Deinit audio mgr & unload library */
    if (u->audio_mgr.deinit) {
        if (u->audio_mgr.deinit(&u->audio_mgr.data) != AUDIO_RET_OK) {
            pa_log_error("audio_mgr deinit failed");
        }
    }
    if (u->audio_mgr.dl_handle) {
        dlclose(u->audio_mgr.dl_handle);
    }

    pa_xfree(u);

    pa_log_info("policy module is unloaded\n");
}
