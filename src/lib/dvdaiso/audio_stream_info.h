/*
* DVD-Audio Decoder plugin
* Copyright (c) 2009-2024 Maxim V.Anisiutkin <maxim.anisiutkin@gmail.com>
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

#include <stdint.h>
#include "dvda_block.h"

enum stream_id_e {UNK_STREAM_ID = 0, PCM_STREAM_ID = 0xa0, MLP_STREAM_ID = 0xa1};
enum stream_type_e {STREAM_TYPE_TRUEHD = 0xba, STREAM_TYPE_MLP = 0xbb};
enum chmode_e {CHMODE_BOTH = 0, CHMODE_TWOCH = 1, CHMODE_MULCH = 2};

typedef struct {
	const uint32_t group1_channel_id[4];
	const uint32_t group2_channel_id[4];
	const char*    group1_channel_name[4];
	const char*    group2_channel_name[4];
	const int      group1_channels;
	const int      group2_channels;
} MLPPCM_ASSIGNMENT;

typedef struct {
	const uint32_t channel_id[2];
	const char*    channel_name[2];
	const int      channels;
} TRUEHD_ASSIGNMENT;

enum {
	SPEAKER_FRONT_LEFT            = 0x1,
	SPEAKER_FRONT_RIGHT           = 0x2,
	SPEAKER_FRONT_CENTER          = 0x4,
	SPEAKER_LOW_FREQUENCY         = 0x8,
	SPEAKER_BACK_LEFT             = 0x10,
	SPEAKER_BACK_RIGHT            = 0x20,
	SPEAKER_FRONT_LEFT_OF_CENTER  = 0x40,
	SPEAKER_FRONT_RIGHT_OF_CENTER = 0x80,
 	SPEAKER_BACK_CENTER           = 0x100,
	SPEAKER_SIDE_LEFT             = 0x200,
	SPEAKER_SIDE_RIGHT            = 0x400,
	SPEAKER_TOP_CENTER            = 0x800,
	SPEAKER_TOP_FRONT_LEFT        = 0x1000,
	SPEAKER_TOP_FRONT_CENTER      = 0x2000,
	SPEAKER_TOP_FRONT_RIGHT       = 0x4000,
	SPEAKER_TOP_BACK_LEFT         = 0x8000,
	SPEAKER_TOP_BACK_CENTER       = 0x10000,
	SPEAKER_TOP_BACK_RIGHT        = 0x20000
};

class audio_stream_info_t {
public:
	static const MLPPCM_ASSIGNMENT mlppcm_table[21];
	static const TRUEHD_ASSIGNMENT truehd_table[13];
	stream_id_e stream_id;
	stream_type_e stream_type;
	bool is_atmos;
	int channel_arrangement;
	uint64_t channel_layout;
	int group1_channels;
	int group1_bits;
	int group1_samplerate;
	int group2_channels;
	int group2_bits;
	int group2_samplerate;
	int bitrate;
	bool can_downmix;
	bool is_vbr;
	int sync_offset;
	audio_stream_info_t();
	operator bool();
	const char* get_channel_name(int channel);
	uint32_t get_wfx_channels();
	double estimate_compression();
};
