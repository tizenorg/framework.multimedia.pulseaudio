#ifndef foopulsetizenaudiofoo
#define foopulsetizenaudiofoo

/***
  This file is part of PulseAudio.

  Copyright 2013 Hyunseok Lee <hs7388.lee@samsung.com>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

/* FIXME : This file should be separated from PA in future */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define AUDIO_REVISION                  1

/* Error code */

#define AUDIO_IS_ERROR(ret)             (ret < 0)

typedef enum audio_return {
    AUDIO_RET_OK                        = 0,
    AUDIO_RET_USE_HW_CONTROL            = (int32_t)0x40001000,
    AUDIO_ERR_UNDEFINED                 = (int32_t)0x80001000,
    AUDIO_ERR_RESOURCE                  = (int32_t)0x80001001,
    AUDIO_ERR_PARAMETER                 = (int32_t)0x80001002,
    AUDIO_ERR_IOCTL                     = (int32_t)0x80001003,
} audio_return_t ;


/* Session */
typedef enum audio_session {
    AUDIO_SESSION_MEDIA,
    AUDIO_SESSION_VOICECALL,
    AUDIO_SESSION_VIDEOCALL,
    AUDIO_SESSION_VOIP,
    AUDIO_SESSION_FMRADIO,
    AUDIO_SESSION_CAMCORDER,
    AUDIO_SESSION_NOTIFICATION,
    AUDIO_SESSION_ALARM,
    AUDIO_SESSION_EMERGENCY,
    AUDIO_SESSION_VOICE_RECOGNITION,
    AUDIO_SESSION_MAX
} audio_session_t;

/* Sub session */
typedef enum audio_subsession {
    AUDIO_SUBSESSION_NONE,
    AUDIO_SUBSESSION_VOICE,
    AUDIO_SUBSESSION_RINGTONE,
    AUDIO_SUBSESSION_MEDIA,
    AUDIO_SUBSESSION_VR_INIT,
    AUDIO_SUBSESSION_VR_NORMAL,
    AUDIO_SUBSESSION_VR_DRIVE,
    AUDIO_SUBSESSION_STEREO_REC,
    AUDIO_SUBSESSION_AM_PLAY,
    AUDIO_SUBSESSION_AM_REC,
    AUDIO_SUBSESSION_MAX
} audio_subsession_t;


/* Direction */
typedef enum audio_direction {
    AUDIO_DIRECTION_NONE,
    AUDIO_DIRECTION_IN,                 /**< Capture */
    AUDIO_DIRECTION_OUT,                /**< Playback */
} audio_direction_t;


/* Device */

typedef enum audio_device_in {
    AUDIO_DEVICE_IN_NONE,
    AUDIO_DEVICE_IN_MIC,                /**< Device builtin mic. */
    AUDIO_DEVICE_IN_WIRED_ACCESSORY,    /**< Wired input devices */
    AUDIO_DEVICE_IN_BT_SCO,             /**< Bluetooth SCO device */
    AUDIO_DEVICE_IN_MAX,
} audio_device_in_t;

typedef enum audio_device_out {
    AUDIO_DEVICE_OUT_NONE,
    AUDIO_DEVICE_OUT_SPEAKER,           /**< Device builtin speaker */
    AUDIO_DEVICE_OUT_RECEIVER,          /**< Device builtin receiver */
    AUDIO_DEVICE_OUT_WIRED_ACCESSORY,   /**< Wired output devices such as headphone, headset, and so on. */
    AUDIO_DEVICE_OUT_BT_SCO,            /**< Bluetooth SCO device */
    AUDIO_DEVICE_OUT_BT_A2DP,           /**< Bluetooth A2DP device */
    AUDIO_DEVICE_OUT_DOCK,              /**< DOCK device */
    AUDIO_DEVICE_OUT_HDMI,              /**< HDMI device */
    AUDIO_DEVICE_OUT_MIRRORING,         /**< MIRRORING device */
    AUDIO_DEVICE_OUT_USB_AUDIO,         /**< USB Audio device */
    AUDIO_DEVICE_OUT_MULTIMEDIA_DOCK,   /**< Multimedia DOCK device */
    AUDIO_DEVICE_OUT_MAX,
} audio_device_out_t;

typedef enum audio_route_flag {
    AUDIO_ROUTE_FLAG_NONE               = 0,
    AUDIO_ROUTE_FLAG_DUAL_OUT           = 0x00000001,
    AUDIO_ROUTE_FLAG_NOISE_REDUCTION    = 0x00000002,
    AUDIO_ROUTE_FLAG_EXTRA_VOL          = 0x00000004,
    AUDIO_ROUTE_FLAG_WB                 = 0x00000008,
    AUDIO_ROUTE_FLAG_SVOICE_COMMAND     = 0x00010000,
    AUDIO_ROUTE_FLAG_SVOICE_WAKEUP      = 0x00020000,
} audio_route_flag_t;

typedef enum audio_device_api {
    AUDIO_DEVICE_API_UNKNOWN,
    AUDIO_DEVICE_API_ALSA,
    AUDIO_DEVICE_API_BLUEZ,
} audio_device_api_t;

typedef struct audio_device_alsa_info {
    uint32_t card_idx;
    uint32_t device_idx;
} audio_device_alsa_info_t;

typedef struct audio_device_bluz_info {
    char *protocol;
    uint32_t nrec;
} audio_device_bluez_info_t;

typedef struct audio_device_info {
    audio_device_api_t api;
    union {
        audio_device_alsa_info_t alsa;
        audio_device_bluez_info_t bluez;
    };
} audio_device_info_t;


/* Stream */

typedef enum audio_volume {
    AUDIO_VOLUME_TYPE_SYSTEM,           /**< System volume type */
    AUDIO_VOLUME_TYPE_NOTIFICATION,     /**< Notification volume type */
    AUDIO_VOLUME_TYPE_ALARM,            /**< Alarm volume type */
    AUDIO_VOLUME_TYPE_RINGTONE,         /**< Ringtone volume type */
    AUDIO_VOLUME_TYPE_MEDIA,            /**< Media volume type */
    AUDIO_VOLUME_TYPE_CALL,             /**< Call volume type */
    AUDIO_VOLUME_TYPE_VOIP,             /**< VOIP volume type */
    AUDIO_VOLUME_TYPE_SVOICE,           /**< SVOICE volume type */
    AUDIO_VOLUME_TYPE_FIXED,            /**< Volume type for fixed acoustic level */
    AUDIO_VOLUME_TYPE_EXT_JAVA,         /**< External system volume for Java */
    AUDIO_VOLUME_TYPE_MAX,              /**< Volume type count */
} audio_volume_t;

typedef enum audio_gain {
    AUDIO_GAIN_TYPE_DEFAULT,
    AUDIO_GAIN_TYPE_DIALER,
    AUDIO_GAIN_TYPE_TOUCH,
    AUDIO_GAIN_TYPE_AF,
    AUDIO_GAIN_TYPE_SHUTTER1,
    AUDIO_GAIN_TYPE_SHUTTER2,
    AUDIO_GAIN_TYPE_CAMCODING,
    AUDIO_GAIN_TYPE_MIDI,
    AUDIO_GAIN_TYPE_BOOTING,
    AUDIO_GAIN_TYPE_VIDEO,
    AUDIO_GAIN_TYPE_TTS,
    AUDIO_GAIN_TYPE_MAX,
} audio_gain_t;

#if 0 // TODO : need to consider */
typedef enum audio_volume_format {
    AUDIO_VOLUME_FORMAT_LINEAR,     /**< Linear format (double) */
    AUDIO_VOLUME_FORMAT_DECIBEL,    /**< Decibel format (double) */
    AUDIO_VOLUME_FORMAT_PA,         /**< PulseAudio format (pa_volume_t) */
} audio_volume_format_t;

typedef struct audio_volume_value {
    audio_volume_format format;
    union {
        double linear;
        double decibel;
        uint32_t pa;
    } value;
} audio_volume_value_t;
#endif

typedef struct audio_stream_info {
    const char *name;
    uint32_t samplerate;
    uint8_t channels;
    uint32_t volume_type;
    uint32_t gain_type;
} audio_stream_info_t ;


/* Overall */

typedef struct audio_info {
    audio_device_info_t device;
    audio_stream_info_t stream;
} audio_info_t;

int audio_get_revision (void);
audio_return_t audio_init (void **userdata);
audio_return_t audio_deinit (void **userdata);
audio_return_t audio_reset (void **userdata);
audio_return_t audio_get_volume_level_max (void *userdata, uint32_t volume_type, uint32_t *level);
audio_return_t audio_get_volume_level (void *userdata, uint32_t volume_type, uint32_t *level);
audio_return_t audio_get_volume_value (void *userdata, audio_info_t *info, uint32_t volume_type, uint32_t level, double *value);
audio_return_t audio_set_volume_level (void *userdata, audio_info_t *info, uint32_t volume_type, uint32_t level);
audio_return_t audio_get_mute (void *userdata, audio_info_t *info, uint32_t volume_type, uint32_t direction, uint32_t *mute);
audio_return_t audio_set_mute (void *userdata, audio_info_t *info, uint32_t volume_type, uint32_t direction, uint32_t mute);
audio_return_t audio_set_session (void *userdata, uint32_t session, uint32_t subsession);
audio_return_t audio_set_route (void *userdata, uint32_t session, uint32_t subsession, uint32_t device_in, uint32_t device_out, uint32_t route_flag);
audio_return_t audio_set_ecrx_device(void *userdata);

#endif
