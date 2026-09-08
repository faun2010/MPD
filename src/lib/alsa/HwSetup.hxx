// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#ifndef MPD_ALSA_HW_SETUP_HXX
#define MPD_ALSA_HW_SETUP_HXX

#include "pcm/Export.hxx"

#include <alsa/asoundlib.h>

struct AudioFormat;

namespace Alsa {

struct HwResult {
	snd_pcm_format_t format;
	snd_pcm_uframes_t buffer_size, period_size;
	bool can_pause;
};

/**
 * Initialize and refine the access mode, sample format, channels and rate.
 * This does not apply hardware parameters or prepare/start the PCM.
 *
 * @param audio_format updated to the negotiated input format
 * @param params updated with the negotiated export packing
 * @return the negotiated ALSA sample (word) rate
 */
unsigned
ConfigureHwFormat(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwparams,
		  AudioFormat &audio_format, PcmExport::Params &params);

/**
 * Wrapper for snd_pcm_hw_params().
 *
 * @param buffer_time the configured buffer time, or 0 if not configured
 * @param period_time the configured period time, or 0 if not configured
 * @param audio_format an #AudioFormat to be configured (or modified)
 * by this function
 * @param params to be modified by this function
 */
HwResult
SetupHw(snd_pcm_t *pcm,
	unsigned buffer_time, unsigned period_time,
	AudioFormat &audio_format, PcmExport::Params &params);

} // namespace Alsa

#endif
