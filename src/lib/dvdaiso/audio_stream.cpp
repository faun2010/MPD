/*
* DVD-Audio Decoder plugin
* Copyright (c) 2009-2023 Maxim V.Anisiutkin <maxim.anisiutkin@gmail.com>
*
* DVD-Audio Decoder is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* DVD-Audio Decoder is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with FFmpeg; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "mlp_audio_stream.h"
#include "pcm_audio_stream.h"

int32_t audio_stream_t::conv_sample(double sample) {
	double lim = (double)(((uint32_t)1 << ((info.group1_bits > 16 ? 32 : 16) - 1)) - 1);
	sample += 0.5;
	if (sample > lim)
		sample = lim;
	else if (sample < -lim)
		sample = -lim;
	return (int32_t)sample;
}

void audio_stream_t::reorder_channels(uint8_t* data, int* data_size) {
	if (info.stream_id == MLP_STREAM_ID && info.stream_type == STREAM_TYPE_TRUEHD)
		return;
	if (info.channel_arrangement == 33) {
		switch (info.group1_bits) {
		case 16:
			for (int offset = 0; offset < *data_size; offset += (info.group1_channels + info.group2_channels) * sizeof(int16_t)) {
				int16_t* sample = (int16_t*)(data + offset);
				sample[0] = 0;
			}
			break;
		case 20:
		case 24:
			for (int offset = 0; offset < *data_size; offset += (info.group1_channels + info.group2_channels) * sizeof(int32_t)) {
				int32_t* sample = (int32_t*)(data + offset);
				sample[0] = 0;
				sample[1] = 0;
				sample[2] = 0;
				//sample[3] = 0;
			}
			break;
		default:
			break;
		}
		return;
	}
	if (info.channel_arrangement < 18)
		return;
	switch (info.group1_bits) {
	case 16:
		for (int offset = 0; offset < *data_size; offset += (info.group1_channels + info.group2_channels) * sizeof(int16_t)) {
			int16_t* sample = (int16_t*)(data + offset);
			int16_t Ls = sample[2];
			int16_t Rs = sample[3];
			for (int i = 0; i < info.group2_channels; i++)
				sample[2 + i] = sample[info.group1_channels + i];
			sample[2 + info.group2_channels + 0] = Ls;
			sample[2 + info.group2_channels + 1] = Rs;
		}
		break;
	case 20:
	case 24:
		for (int offset = 0; offset < *data_size; offset += (info.group1_channels + info.group2_channels) * sizeof(int32_t)) {
			int32_t* sample = (int32_t*)(data + offset);
			int32_t Ls = sample[2];
			int32_t Rs = sample[3];
			for (int i = 0; i < info.group2_channels; i++)
				sample[2 + i] = sample[info.group1_channels + i];
			sample[2 + info.group2_channels + 0] = Ls;
			sample[2 + info.group2_channels + 1] = Rs;
		}
		break;
	default:
		break;
	}
}

void audio_stream_t::set_downmix_coef() {
	// Ldmx
	LR_dmx_coef[0][0] = +0.500; // Lf
	LR_dmx_coef[1][0] = +0.000; // Rf
	LR_dmx_coef[2][0] = +0.354; // C
	LR_dmx_coef[3][0] = +0.177; // LFE
	LR_dmx_coef[4][0] = +0.250; // Ls
	LR_dmx_coef[5][0] = +0.000; // Rs
	LR_dmx_coef[6][0] = +0.000;
	LR_dmx_coef[7][0] = +0.000;
	// Rdmx
	LR_dmx_coef[0][1] = +0.000; // Lf
	LR_dmx_coef[1][1] = +0.500; // Rf
	LR_dmx_coef[2][1] = +0.354; // C
	LR_dmx_coef[3][1] = +0.177; // LFE
	LR_dmx_coef[4][1] = +0.000; // Ls
	LR_dmx_coef[5][1] = +0.250; // Rs
	LR_dmx_coef[6][1] = +0.000;
	LR_dmx_coef[7][1] = +0.000;
}

void audio_stream_t::set_downmix_coef(double dmx_coef[8][2]) {
	for (int ch = 0; ch < 8; ch++) {
		LR_dmx_coef[ch][0] = dmx_coef[ch][0];
		LR_dmx_coef[ch][1] = dmx_coef[ch][1];
	}
}

void audio_stream_t::downmix_channels(uint8_t* data, int* data_size) {
	int channels = info.group1_channels + info.group2_channels;
	int dmx_offset = 0;
	switch (info.group1_bits) {
	case 16:
		for (int offset = 0; offset < *data_size; offset += channels * sizeof(int16_t)) {
			double L = 0.0;
			double R = 0.0;
			for (int ch = 0; ch < channels; ch++) {
				int16_t sample = *(int16_t*)(data + offset + ch * sizeof(int16_t));
				if (ch < 8) {
					L += (double)sample * LR_dmx_coef[ch][0];
					R += (double)sample * LR_dmx_coef[ch][1];
				}
			}
			*(int16_t*)(data + dmx_offset) = (int16_t)conv_sample(L);
			dmx_offset +=  sizeof(int16_t);
			*(int16_t*)(data + dmx_offset) = (int16_t)conv_sample(R);
			dmx_offset +=  sizeof(int16_t);
		}
		break;
	case 20:
	case 24:
		for (int offset = 0; offset < *data_size; offset += channels * sizeof(int32_t)) {
			double L = 0.0;
			double R = 0.0;
			for (int ch = 0; ch < channels; ch++) {
				if (ch < 8) {
					int32_t sample = *(int32_t*)(data + offset + ch * sizeof(int32_t));
					L += (double)sample * LR_dmx_coef[ch][0];
					R += (double)sample * LR_dmx_coef[ch][1];
				}
			}
			*(int32_t*)(data + dmx_offset) = (int32_t)conv_sample(L);
			dmx_offset +=  sizeof(int32_t);
			*(int32_t*)(data + dmx_offset) = (int32_t)conv_sample(R);
			dmx_offset +=  sizeof(int32_t);
		}
		break;
	default:
		break;
	}
	*data_size = dmx_offset;
}

std::unique_ptr<audio_stream_t> audio_stream_t::create_stream(stream_id_e stream_id) {
	switch (stream_id) {
	case MLP_STREAM_ID:
		return std::make_unique<mlp_audio_stream_t>();
	case PCM_STREAM_ID:
		return std::make_unique<pcm_audio_stream_t>();
	default:
		break;
	}
	return nullptr;
}
