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

#pragma once

#include "audio_stream_info.h"
#include <string.h>
#include <memory>

class audio_stream_t {
	static constexpr int AVG_BITRATE_SIZE = 256;
	int instant_bits_read[AVG_BITRATE_SIZE];
	int instant_bits_decoded[AVG_BITRATE_SIZE];
	int instant_bit_index;
	int avg_bits_read;
	int avg_bits_decoded;
protected:
	audio_stream_info_t info;
	bool do_check;
	bool do_downmix;
	double LR_dmx_coef[8][2];
	int64_t bits_read;
	int64_t bits_decoded;
	void reset_stats() {
		memset(instant_bits_read, 0, sizeof(instant_bits_read));
		memset(instant_bits_decoded, 0, sizeof(instant_bits_decoded));
		instant_bit_index = 0;
		avg_bits_read = avg_bits_decoded = 0;
		bits_read = bits_decoded = 0;
	}
	void update_stats(int decoder_bits_read, int decoder_bits_decoded) {
		if (instant_bit_index >= AVG_BITRATE_SIZE)
			instant_bit_index = 0;
		avg_bits_read -= instant_bits_read[instant_bit_index];
		instant_bits_read[instant_bit_index] = decoder_bits_read;
		avg_bits_read += instant_bits_read[instant_bit_index];
		avg_bits_decoded -= instant_bits_decoded[instant_bit_index];
		instant_bits_decoded[instant_bit_index] = decoder_bits_decoded;
		avg_bits_decoded += instant_bits_decoded[instant_bit_index];
		instant_bit_index++;
		bits_read += decoder_bits_read;
		bits_decoded += decoder_bits_decoded;
	}
	int32_t conv_sample(double sample);
	void reorder_channels(uint8_t* data, int* data_size);
	void downmix_channels(uint8_t* data, int* data_size);
public:
	static const int MAX_CHUNK_SIZE = 2 * 4096 + 4;
	static const int RETCODE_REINIT = -128;
	static const int RETCODE_EXCEPT = -256;
	static std::unique_ptr<audio_stream_t> create_stream(stream_id_e stream_id);
	audio_stream_t() {
		do_check = false;
		do_downmix = false;
		bits_read = 0;
		bits_decoded = 0;
		reset_stats();
	}
	virtual ~audio_stream_t() {
	}
	audio_stream_info_t get_info() {
		return info;
	}
	stream_id_e get_stream_id() {
		return info.stream_id;
	}
	bool get_downmix() {
		return do_downmix;
	}
	void set_check(bool check) {
		do_check = check;
	}
	double get_compression() {
		if (bits_read > 0 && bits_decoded > 0) {
			return (double)bits_decoded / (double)bits_read;
		}
		return info.estimate_compression();
	}
	double get_instant_bitrate() {
		return (avg_bits_decoded > 0 ? (double)avg_bits_read / (double)avg_bits_decoded : 1.0) * (double)info.bitrate;
	}
	void set_downmix_coef();
	void set_downmix_coef(double dmx_coef[8][2]);
	virtual audio_stream_info_t get_info(uint8_t* buf, int buf_size) = 0;
	virtual int init(uint8_t* buf, int buf_size, bool downmix, bool reset_statistics = true) = 0;
	virtual int decode(uint8_t* data, int* data_size, uint8_t* buf, int buf_size) = 0;
	virtual int resync(uint8_t* buf, int buf_size) = 0;
};
