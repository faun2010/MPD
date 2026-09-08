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

#include "audio_stream.h"

struct mlp_mh_t {
	uint32_t major_sync;
	uint8_t  group2_bits : 4;
	uint8_t  group1_bits : 4;
	uint8_t  group2_samplerate : 4;
	uint8_t  group1_samplerate : 4;
	uint8_t  padding1;
	uint8_t  channel_assignment;
};

class mlp_audio_stream_t : public audio_stream_t {
	class mlp_ctx_t;
	mlp_ctx_t* ctx;
public:
	mlp_audio_stream_t();
	~mlp_audio_stream_t();
	audio_stream_info_t get_info(uint8_t* buf, int buf_size) override;
	int init(uint8_t* buf, int buf_size, bool downmix, bool reset_statistics = true) override;
	int decode(uint8_t* data, int* data_size, uint8_t* buf, int buf_size) override;
	int resync(uint8_t* buf, int buf_size) override;
private:
	int decode_packet(uint8_t* data, int* data_size);
};
