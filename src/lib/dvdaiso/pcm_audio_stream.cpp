/*
* DVD-Audio Decoder plugin
* Copyright (c) 2009-2025 Maxim V.Anisiutkin <maxim.anisiutkin@gmail.com>
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

#include "pcm_audio_stream.h"

typedef struct {
	uint16_t first_audio_frame;
	uint8_t  padding1;
	uint8_t  group2_bits : 4;
	uint8_t  group1_bits : 4;
	uint8_t  group2_samplerate : 4;
	uint8_t  group1_samplerate : 4;
	uint8_t  padding2;
	uint8_t  channel_assignment;
	uint8_t  padding3;
	uint8_t  cci;
} pcm_header_t;

audio_stream_info_t pcm_audio_stream_t::get_info(uint8_t* buf, int buf_size) {
	audio_stream_info_t si;
	if (buf_size >= (int)sizeof(pcm_header_t)) {
		auto ph = (pcm_header_t*)buf;
		if (ph->channel_assignment <= 20) {
			si.stream_id = PCM_STREAM_ID;
			si.channel_arrangement = ph->channel_assignment;
			si.channel_layout = si.get_wfx_channels();
			si.group1_channels = audio_stream_info_t::mlppcm_table[si.channel_arrangement].group1_channels;
			si.group2_channels = audio_stream_info_t::mlppcm_table[si.channel_arrangement].group2_channels;
			si.group1_bits = ph->group1_bits > 2 ? 0 : 16 + ph->group1_bits * 4;
			si.group2_bits = ph->group2_bits > 2 ? 0 : 16 + ph->group2_bits * 4;
			si.group1_samplerate = (ph->group1_samplerate & 7) > 2 ? 0 : ph->group1_samplerate & 8 ? 44100 << (ph->group1_samplerate & 7) : 48000 << (ph->group1_samplerate & 7);
			si.group2_samplerate = (ph->group2_samplerate & 7) > 2 ? 0 : ph->group2_samplerate & 8 ? 44100 << (ph->group2_samplerate & 7) : 48000 << (ph->group2_samplerate & 7);
			si.bitrate = si.group1_channels * si.group1_bits * si.group1_samplerate + si.group2_channels * si.group2_bits * si.group2_samplerate;
			si.can_downmix = false;
			si.is_vbr = false;
			si.sync_offset = 0;
		}
	}
	return si;
}

int pcm_audio_stream_t::init(uint8_t* buf, int buf_size, bool downmix, bool reset_statistics) {
	info = get_info(buf, buf_size);
	if (!info) {
		return -1;
	}
	raw_group2_index = 0;
	raw_group2_factor = info.group2_channels > 0 ? info.group1_samplerate / info.group2_samplerate : 1;
	raw_group1_size = info.group1_channels * info.group1_bits / 4;
	raw_group2_size = info.group2_channels * info.group2_bits / 4;
	pcm_sample_size = info.group1_bits > 16 ? 4 : 2;
	pcm_group1_size = 2 * info.group1_channels * pcm_sample_size;
	pcm_group2_size = 2 * info.group2_channels * pcm_sample_size;
	do_downmix = downmix;
	if (downmix) {
		set_downmix_coef();
	}
	if (reset_statistics) {
		reset_stats();
	}
	return 0;
}

int pcm_audio_stream_t::decode(uint8_t* data, int* data_size, uint8_t* buf, int buf_size) {
	auto buf_out = data;
	auto buf_inp = buf;
	if (buf_size > DVD_BLOCK_SIZE) {
		buf_size = DVD_BLOCK_SIZE;
	}
	while (buf_inp + raw_group1_size + (raw_group2_index == 0 ? raw_group2_size : 0) <= buf + buf_size) {
		int pcm_byte_index;
		pcm_byte_index = 0;
		if (raw_group2_index == 0) {
			for (int i = 0; i < 2 * info.group2_channels; i++) {
				switch (info.group2_bits) {
				case 16:
					if (info.group1_bits > 16) {
						pcm_group2_pack[pcm_byte_index++] = 0;
						pcm_group2_pack[pcm_byte_index++] = 0;
					}
					pcm_group2_pack[pcm_byte_index++] = buf_inp[2 * i + 1];
					pcm_group2_pack[pcm_byte_index++] = buf_inp[2 * i];
					break;
				case 20:
					pcm_group2_pack[pcm_byte_index++] = 0;
					if (i % 2) {
						pcm_group2_pack[pcm_byte_index++] = buf_inp[4 * info.group2_channels + i / 2] & 0x0f;
					}
					else {
						pcm_group2_pack[pcm_byte_index++] = buf_inp[4 * info.group2_channels + i / 2] >> 4;
					}
					pcm_group2_pack[pcm_byte_index++] = buf_inp[2 * i + 1];
					pcm_group2_pack[pcm_byte_index++] = buf_inp[2 * i];
					break;
				case 24:
					pcm_group2_pack[pcm_byte_index++] = 0;
					pcm_group2_pack[pcm_byte_index++] = buf_inp[4 * info.group2_channels + i];
					pcm_group2_pack[pcm_byte_index++] = buf_inp[2 * i + 1];
					pcm_group2_pack[pcm_byte_index++] = buf_inp[2 * i];
					break;
				default:
					break;
				}
			}
			buf_inp += raw_group2_size;
		}
		raw_group2_index++;
		if (raw_group2_index == raw_group2_factor) {
			raw_group2_index = 0;
		}
		pcm_byte_index = 0;
		for (int i = 0; i < 2 * info.group1_channels; i++) {
			switch (info.group1_bits) {
			case 16:
				pcm_group1_pack[pcm_byte_index++] = buf_inp[2 * i + 1];
				pcm_group1_pack[pcm_byte_index++] = buf_inp[2 * i];
				break;
			case 20:
				pcm_group1_pack[pcm_byte_index++] = 0;
				if (i % 2) {
					pcm_group1_pack[pcm_byte_index++] = buf_inp[4 * info.group1_channels + i / 2] << 4;
				}
				else {
					pcm_group1_pack[pcm_byte_index++] = buf_inp[4 * info.group1_channels + i / 2] & 0xf0;
				}
				pcm_group1_pack[pcm_byte_index++] = buf_inp[2 * i + 1];
				pcm_group1_pack[pcm_byte_index++] = buf_inp[2 * i];
				break;
			case 24:
				pcm_group1_pack[pcm_byte_index++] = 0;
				pcm_group1_pack[pcm_byte_index++] = buf_inp[4 * info.group1_channels + i];
				pcm_group1_pack[pcm_byte_index++] = buf_inp[2 * i + 1];
				pcm_group1_pack[pcm_byte_index++] = buf_inp[2 * i];
				break;
			default:
				break;
			}
		}
		buf_inp += raw_group1_size;
		memcpy(buf_out, pcm_group1_pack, pcm_group1_size / 2);
		buf_out += pcm_group1_size / 2;
		memcpy(buf_out, pcm_group2_pack, pcm_group2_size / 2);
		buf_out += pcm_group2_size / 2;
		memcpy(buf_out, pcm_group1_pack + pcm_group1_size / 2, pcm_group1_size / 2);
		buf_out += pcm_group1_size / 2;
		memcpy(buf_out, pcm_group2_pack + pcm_group2_size / 2, pcm_group2_size / 2);
		buf_out += pcm_group2_size / 2;
	}
	*data_size = (int)(buf_out - data);
	int bytes_decoded = (int)(buf_inp - buf);
	int buf_bits_read = 8 * bytes_decoded;
	int buf_samples_decoded = (*data_size) / pcm_sample_size / (info.group1_channels + info.group2_channels);
	int buf_bits_decoded = buf_samples_decoded * (info.group1_channels * info.group1_bits + info.group2_channels * info.group2_bits * info.group2_samplerate / info.group1_samplerate);
	if (!do_downmix) {
		reorder_channels(data, data_size);
	}
	else if (!info.can_downmix) {
		reorder_channels(data, data_size);
		downmix_channels(data, data_size);
	}
	update_stats(buf_bits_read, buf_bits_decoded);
	return bytes_decoded;
}

int pcm_audio_stream_t::resync([[maybe_unused]]uint8_t* buf, [[maybe_unused]]int buf_size) {
	return 0;
}
