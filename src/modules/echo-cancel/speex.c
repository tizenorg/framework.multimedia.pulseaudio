/***
    This file is part of PulseAudio.

    Copyright 2010 Wim Taymans <wim.taymans@gmail.com>

    Contributor: Arun Raghavan <arun.raghavan@collabora.co.uk>

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

#include <pulsecore/modargs.h>
#include "echo-cancel.h"

/* should be between 10-20 ms */
#define DEFAULT_FRAME_SIZE_MS 20
/* should be between 100-500 ms */
#define DEFAULT_FILTER_SIZE_MS 200

static const char* const valid_modargs[] = {
    "frame_size_ms",
    "filter_size_ms",
    NULL
};

static void pa_speex_ec_fixate_spec(pa_sample_spec *source_ss, pa_channel_map *source_map,
				    pa_sample_spec *sink_ss, pa_channel_map *sink_map)
{
    source_ss->format = PA_SAMPLE_S16NE;
    source_ss->channels = 1; //add for SLP
    source_ss->rate = 8000;//add for SLP
    pa_channel_map_init_mono(source_map);//add for SLP

    *sink_ss = *source_ss;
    *sink_map = *source_map;
}

pa_bool_t pa_speex_ec_init(pa_echo_canceller *ec,
                           pa_sample_spec *source_ss, pa_channel_map *source_map,
                           pa_sample_spec *sink_ss, pa_channel_map *sink_map,
                           uint32_t *blocksize, const char *args)
{
    int framelen, y, rate;
    uint32_t frame_size_ms, filter_size_ms;
    pa_modargs *ma;

    if (!(ma = pa_modargs_new(args, valid_modargs))) {
        pa_log("Failed to parse submodule arguments.");
        goto fail;
    }

    filter_size_ms = DEFAULT_FILTER_SIZE_MS;
    if (pa_modargs_get_value_u32(ma, "filter_size_ms", &filter_size_ms) < 0 || filter_size_ms < 1 || filter_size_ms > 2000) {
        pa_log("Invalid filter_size_ms specification");
        goto fail;
    }
    else {
    	pa_log ("filter size : %d ms", filter_size_ms);
    }

    frame_size_ms = DEFAULT_FRAME_SIZE_MS;
    if (pa_modargs_get_value_u32(ma, "frame_size_ms", &frame_size_ms) < 0 || frame_size_ms < 1 || frame_size_ms > 200) {
        pa_log("Invalid frame_size_ms specification");
        goto fail;
    }

    pa_speex_ec_fixate_spec(source_ss, source_map, sink_ss, sink_map);

    rate = source_ss->rate;
    framelen = (rate * frame_size_ms) / 1000;
    /* framelen should be a power of 2, round down to nearest power of two */
    y = 1 << ((8 * sizeof (int)) - 2);
    while (y > framelen)
      y >>= 1;
    framelen = y;

    *blocksize = framelen * pa_frame_size (source_ss);

    pa_log_debug ("Using framelen %d, blocksize %u, channels %d, rate %d", framelen, *blocksize, source_ss->channels, source_ss->rate);

    ec->params.priv.speex.state = speex_echo_state_init_mc (framelen, (rate * filter_size_ms) / 1000, source_ss->channels, source_ss->channels);

    if (!ec->params.priv.speex.state)
	goto fail;

    speex_echo_ctl(ec->params.priv.speex.state, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);

    /* "init. preprocessor" moved from "module-echo-cancel.c" */
    if (ec->tx_agc || ec->tx_denoise || ec->echo_suppress) {
    	spx_int32_t tmp;

    	if (source_ss->channels != 1) {
    		pa_log ("AGC, denoising and echo suppression only work with channels=1");
    		goto fail;
    	}
    	ec->params.priv.speex.pp_state = speex_preprocess_state_init (*blocksize / pa_frame_size(source_ss), source_ss->rate);

    	if (ec->tx_agc) {
    		/*
    		 * speex 1.2rc1 does not support AGC on fixed-point mode.
    		 * So it does not work with SLP (libspeex in SLP compiled with fixed-point mode)
    		 */
			tmp = ec->tx_agc;
			if ( speex_preprocess_ctl (ec->params.priv.speex.pp_state, SPEEX_PREPROCESS_SET_AGC, &tmp) < 0) {
				pa_log ("Speex preprocess set AGC failed");
				goto fail;
			}
    	}

    	tmp = ec->tx_denoise;
    	if ( speex_preprocess_ctl (ec->params.priv.speex.pp_state, SPEEX_PREPROCESS_SET_DENOISE, &tmp) < 0 ) {
    		pa_log ("Speex preprocess set denoising failed");
    		goto fail;
    	}

    	if (ec->echo_suppress) {
    		if (ec->echo_suppress_attenuation) {
    			if (speex_preprocess_ctl (ec->params.priv.speex.pp_state, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &ec->echo_suppress_attenuation) < 0 ) {
    	    		pa_log ("Speex preprocess set echo suppress failed");
    	    		goto fail;
    			}
    		}
    		if (ec->echo_suppress_attenuation_active) {
    			if (speex_preprocess_ctl (ec->params.priv.speex.pp_state, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &ec->echo_suppress_attenuation_active) < 0) {
    				pa_log ("Speex preprocess set echo suppress active failed");
    				goto fail;
    			}
    		}
    		if (speex_preprocess_ctl (ec->params.priv.speex.pp_state, SPEEX_PREPROCESS_SET_ECHO_STATE, ec->params.priv.speex.state) < 0) {
    			pa_log ("Speex preprocess set echo state failed");
    			goto fail;
    		}
    	}
    }

    pa_modargs_free(ma);
    return TRUE;

fail:
    if (ma)
    	pa_modargs_free(ma);
    return FALSE;
}

void pa_speex_ec_run(pa_echo_canceller *ec, const uint8_t *rec, const uint8_t *play, uint8_t *out)
{
    speex_echo_cancellation(ec->params.priv.speex.state, (const spx_int16_t *) rec, (const spx_int16_t *) play, (spx_int16_t *) out);

    /* preprocessor is run after AEC, This is not a mistake ! :: moved from "module-echo-cancel.c" */
    if (ec->params.priv.speex.pp_state) {
    	speex_preprocess_run (ec->params.priv.speex.pp_state, (spx_int16_t *) out);
    }
}

void pa_speex_ec_done(pa_echo_canceller *ec)
{
	/* moved from "module-echo-cancel.c" */
	if (ec->params.priv.speex.pp_state)
		speex_preprocess_state_destroy (ec->params.priv.speex.pp_state);
	ec->params.priv.speex.pp_state = NULL;

    if (ec->params.priv.speex.state)
    	speex_echo_state_destroy (ec->params.priv.speex.state);
    ec->params.priv.speex.state = NULL;
}
