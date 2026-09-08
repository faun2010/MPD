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

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/intreadwrite.h>
}

#include "mlp_audio_stream.h"
#include <algorithm>
#include <cstring>

typedef struct MLPHeaderInfo {
	int stream_type;                        ///< 0xBB for MLP, 0xBA for TrueHD
	int header_size;                        ///< Size of the major sync header, in bytes

	int group1_bits;                        ///< The bit depth of the first substream
	int group2_bits;                        ///< Bit depth of the second substream (MLP only)

	int group1_samplerate;                  ///< Sample rate of first substream
	int group2_samplerate;                  ///< Sample rate of second substream (MLP only)

	int channel_arrangement;

	int channel_modifier_thd_stream0;       ///< Channel modifier for substream 0 of TrueHD streams ("2-channel presentation")
	int channel_modifier_thd_stream1;       ///< Channel modifier for substream 1 of TrueHD streams ("6-channel presentation")
	int channel_modifier_thd_stream2;       ///< Channel modifier for substream 2 of TrueHD streams ("8-channel presentation")

	int channels_mlp;                       ///< Channel count for MLP streams
	int channels_thd_stream1;               ///< Channel count for substream 1 of TrueHD streams ("6-channel presentation")
	int channels_thd_stream2;               ///< Channel count for substream 2 of TrueHD streams ("8-channel presentation")
	uint64_t channel_layout_mlp;            ///< Channel layout for MLP streams
	uint64_t channel_layout_thd_stream1;    ///< Channel layout for substream 1 of TrueHD streams ("6-channel presentation")
	uint64_t channel_layout_thd_stream2;    ///< Channel layout for substream 2 of TrueHD streams ("8-channel presentation")

	int access_unit_size;                   ///< Number of samples per coded frame
	int access_unit_size_pow2;              ///< Next power of two above number of samples per frame

	int is_vbr;                             ///< Stream is VBR instead of CBR
	int peak_bitrate;                       ///< Peak bitrate for VBR, actual bitrate (==peak) for CBR

	int num_substreams;                     ///< Number of substreams within stream

	int extended_substream_info;            ///< Which substream of substreams carry 16-channel presentation
	int substream_info;                     ///< Which substream of substreams carry 2/6/8-channel presentation
} MLPHeaderInfo;

static const uint8_t mlp_quants[16] = {
	16, 20, 24, 0, 0, 0, 0, 0,
	 0,  0,  0, 0, 0, 0, 0, 0,
};

static const uint8_t mlp_channels[32] = {
	1, 2, 3, 4, 3, 4, 5, 3, 4, 5, 4, 5, 6, 4, 5, 4,
	5, 6, 5, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static const uint64_t mlp_layout[32] = {
	AV_CH_LAYOUT_MONO,
	AV_CH_LAYOUT_STEREO,
	AV_CH_LAYOUT_2_1,
	AV_CH_LAYOUT_QUAD,
	AV_CH_LAYOUT_STEREO | AV_CH_LOW_FREQUENCY,
	AV_CH_LAYOUT_2_1 | AV_CH_LOW_FREQUENCY,
	AV_CH_LAYOUT_QUAD | AV_CH_LOW_FREQUENCY,
	AV_CH_LAYOUT_SURROUND,
	AV_CH_LAYOUT_4POINT0,
	AV_CH_LAYOUT_5POINT0_BACK,
	AV_CH_LAYOUT_SURROUND | AV_CH_LOW_FREQUENCY,
	AV_CH_LAYOUT_4POINT0 | AV_CH_LOW_FREQUENCY,
	AV_CH_LAYOUT_5POINT1_BACK,
	AV_CH_LAYOUT_4POINT0,
	AV_CH_LAYOUT_5POINT0_BACK,
	AV_CH_LAYOUT_SURROUND | AV_CH_LOW_FREQUENCY,
	AV_CH_LAYOUT_4POINT0 | AV_CH_LOW_FREQUENCY,
	AV_CH_LAYOUT_5POINT1_BACK,
	AV_CH_LAYOUT_QUAD | AV_CH_LOW_FREQUENCY,
	AV_CH_LAYOUT_5POINT0_BACK,
	AV_CH_LAYOUT_5POINT1_BACK,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static inline int mlp_samplerate(int in) {
	if (in == 0x0f) {
		return 0;
	}
	return (in & 8 ? 44100 : 48000) << (in & 7);
}

static const uint8_t thd_chancount[13] = {
	//  LR    C   LFE  LRs LRvh  LRc LRrs  Cs   Ts  LRsd  LRw  Cvh  LFE2
			 2,   1,   1,   2,   2,   2,   2,   1,   1,   2,   2,   1,   1
};

static const uint64_t thd_layout[13] = {
	AV_CH_FRONT_LEFT | AV_CH_FRONT_RIGHT,                     // LR
	AV_CH_FRONT_CENTER,                                       // C
	AV_CH_LOW_FREQUENCY,                                      // LFE
	AV_CH_SIDE_LEFT | AV_CH_SIDE_RIGHT,                       // LRs
	AV_CH_TOP_FRONT_LEFT | AV_CH_TOP_FRONT_RIGHT,             // LRvh
	AV_CH_FRONT_LEFT_OF_CENTER | AV_CH_FRONT_RIGHT_OF_CENTER, // LRc
	AV_CH_BACK_LEFT | AV_CH_BACK_RIGHT,                       // LRrs
	AV_CH_BACK_CENTER,                                        // Cs
	AV_CH_TOP_CENTER,                                         // Ts
	AV_CH_SURROUND_DIRECT_LEFT | AV_CH_SURROUND_DIRECT_RIGHT, // LRsd
	AV_CH_WIDE_LEFT | AV_CH_WIDE_RIGHT,                       // LRw
	AV_CH_TOP_FRONT_CENTER,                                   // Cvh
	AV_CH_LOW_FREQUENCY_2,                                    // LFE2
};

static inline int truehd_channels(int chanmap) {
	int channels{ 0 };
	for (auto i = 0; i < 13; i++) {
		channels += thd_chancount[i] * ((chanmap >> i) & 1);
	}
	return channels;
}

static inline uint64_t truehd_layout(int chanmap) {
	uint64_t layout{ 0 };
	for (auto i = 0; i < 13; i++) {
		layout |= thd_layout[i] * ((chanmap >> i) & 1);
	}
	return layout;
}

class MLPHeaderReader {
	const uint8_t* span_data;
	size_t span_bits;
	size_t read_bits;
public:
	MLPHeaderReader(const uint8_t* buf_data, size_t buf_size) {
		auto header_size = get_major_sync_size(buf_data, buf_size);
		span_data = buf_data;
		span_bits = header_size << 3;
		read_bits = 0;
	}
	bool read(MLPHeaderInfo* mh) {
		if (get_bits(24) != 0xf8726f) {
			return false;
		}
		int ratebits, channel_arrangement;
		mh->stream_type = get_bits(8);
		mh->header_size = span_bits >> 3;
		if (mh->stream_type == 0xbb) {
			mh->group1_bits = mlp_quants[get_bits(4)];
			mh->group2_bits = mlp_quants[get_bits(4)];
			ratebits = get_bits(4);
			mh->group1_samplerate = mlp_samplerate(ratebits);
			mh->group2_samplerate = mlp_samplerate(get_bits(4));
			skip_bits(11);
			mh->channel_arrangement = channel_arrangement = get_bits(5);
			mh->channels_mlp = mlp_channels[channel_arrangement];
			mh->channel_layout_mlp = mlp_layout[channel_arrangement];
		}
		else if (mh->stream_type == 0xba) {
			mh->group1_bits = 24;
			mh->group2_bits = 0;
			ratebits = get_bits(4);
			mh->group1_samplerate = mlp_samplerate(ratebits);
			mh->group2_samplerate = 0;
			skip_bits(4);
			mh->channel_modifier_thd_stream0 = get_bits(2);
			mh->channel_modifier_thd_stream1 = get_bits(2);
			mh->channel_arrangement = channel_arrangement = get_bits(5);
			mh->channels_thd_stream1 = truehd_channels(channel_arrangement);
			mh->channel_layout_thd_stream1 = truehd_layout(channel_arrangement);
			mh->channel_modifier_thd_stream2 = get_bits(2);
			channel_arrangement = get_bits(13);
			mh->channels_thd_stream2 = truehd_channels(channel_arrangement);
			mh->channel_layout_thd_stream2 = truehd_layout(channel_arrangement);
		}
		else {
			return false;
		}
		mh->access_unit_size = 40 << (ratebits & 7);
		mh->access_unit_size_pow2 = 64 << (ratebits & 7);
		skip_bits(48);
		mh->is_vbr = get_bits(1);
		mh->peak_bitrate = (get_bits(15) * mh->group1_samplerate + 8) >> 4;
		mh->num_substreams = get_bits(4);
		skip_bits(2);
		mh->extended_substream_info = get_bits(2);
		mh->substream_info = get_bits(8);
		skip_bits(span_bits - (18 << 3));
		return true;
	}
private:
	size_t get_major_sync_size(const uint8_t* buf_data, size_t buf_size) {
		auto has_extension{ false };
		auto extensions{ 0 };
		auto header_size{ 28 };
		if (buf_size < 28) {
			return 0;
		}
		if (AV_RB32(buf_data) == 0xf8726fba) {
			has_extension = buf_data[25] & 1;
			if (has_extension) {
				extensions = buf_data[26] >> 4;
				header_size += 2 + extensions * 2;
			}
		}
		return header_size;
	}
	template<typename T = uint32_t>
	T get_bits(size_t n) {
		T bits{};
		n = std::min(n, sizeof(T) << 3);
		for (auto bit_i = 0u; bit_i < n; bit_i++) {
			if (read_bits < span_bits) {
				T bit_v = (span_data[read_bits / 8] >> (7 - read_bits % 8)) & 1;
				bits |= bit_v << (n - 1 - bit_i);
				read_bits++;
			}
		}
		return bits;
	}
	void skip_bits(size_t n) {
		read_bits += n;
	}
};

typedef struct {
	void* p_avclass;
	void* p_avctx;
	AVChannelLayout downmix_layout;
} mlp_dc_t;

class mlp_audio_stream_t::mlp_ctx_t {
public:
	MLPHeaderInfo         mh;
	mlp_mh_t              mlp_mh;
	const AVCodec*        codec;
	AVCodecContext*       codecCtx;
	AVCodecParserContext* parserCtx;
	AVPacket*             packet;
	AVFrame*              frame;
public:
	mlp_ctx_t() : mh(), mlp_mh(), codec(nullptr), codecCtx(nullptr), parserCtx(nullptr), packet(nullptr), frame(nullptr) {
	}
	virtual ~mlp_ctx_t() {
		avcodec_free_context(&codecCtx);
		av_parser_close(parserCtx);
		av_packet_free(&packet);
		av_frame_free(&frame);
	}
};

mlp_audio_stream_t::mlp_audio_stream_t() {
	ctx = new mlp_ctx_t();
}

mlp_audio_stream_t::~mlp_audio_stream_t() {
	delete ctx;
}

audio_stream_info_t mlp_audio_stream_t::get_info(uint8_t* buf, int buf_size) {
	audio_stream_info_t si;
	if (!ctx) {
		return si;
	}
	auto codec = avcodec_find_decoder(AV_CODEC_ID_MLP);
	if (!codec) {
		return si;
	}
	auto codecCtx = avcodec_alloc_context3(codec);
	auto parserCtx = av_parser_init(codec->id);
	if (!(codecCtx && parserCtx)) {
		goto get_info_exit;
	}
	const uint8_t* out;
	int sync_pos, out_size;
	sync_pos = parserCtx->parser->parser_parse(parserCtx, codecCtx, &out, &out_size, buf, buf_size);
	if (out_size == 0) {
		parserCtx->parser->parser_parse(parserCtx, codecCtx, &out, &out_size, buf + sync_pos, buf_size - sync_pos);
	}
	if (!MLPHeaderReader(buf + sync_pos + 4, buf_size - sync_pos - 4).read(&ctx->mh)) {
		goto get_info_exit;
	}
	switch (ctx->mh.stream_type) {
	case STREAM_TYPE_MLP:
		si.stream_type = STREAM_TYPE_MLP;
		si.is_atmos = false;
		si.channel_arrangement = ctx->mh.channel_arrangement;
		si.channel_layout = ctx->mh.channel_layout_mlp;
		si.group1_channels = audio_stream_info_t::mlppcm_table[si.channel_arrangement].group1_channels;
		si.group1_bits = ctx->mh.group1_bits;
		si.group1_samplerate = ctx->mh.group1_samplerate;
		si.group2_channels = audio_stream_info_t::mlppcm_table[si.channel_arrangement].group2_channels;
		si.group2_bits = ctx->mh.group2_bits;
		si.group2_samplerate = ctx->mh.group2_samplerate;
		break;
	case STREAM_TYPE_TRUEHD:
		si.stream_type = STREAM_TYPE_TRUEHD;
		si.is_atmos = (ctx->mh.num_substreams == 4) && (ctx->mh.substream_info >> 7 == 1);
		si.channel_arrangement = ctx->mh.channel_arrangement;
		if (ctx->mh.channels_thd_stream2) {
			si.channel_layout = ctx->mh.channel_layout_thd_stream2;
			si.group1_channels = ctx->mh.channels_thd_stream2;
		}
		else {
			si.channel_layout = ctx->mh.channel_layout_thd_stream1;
			si.group1_channels = ctx->mh.channels_thd_stream1;
		}
		si.group1_bits = ctx->mh.group1_bits;
		si.group1_samplerate = ctx->mh.group1_samplerate;
		si.group2_channels = 0;
		si.group2_bits = 0;
		si.group2_samplerate = 0;
		break;
	default:
		goto get_info_exit;
	}
	si.stream_id = MLP_STREAM_ID;
	si.bitrate = si.group1_channels * si.group1_bits * si.group1_samplerate + si.group2_channels * si.group2_bits * si.group2_samplerate;
	si.can_downmix = ctx->mh.num_substreams > 1;
	si.is_vbr = ctx->mh.is_vbr == 1;
	si.sync_offset = sync_pos;
get_info_exit:
	av_parser_close(parserCtx);
	avcodec_free_context(&codecCtx);
	return si;
}

int mlp_audio_stream_t::init(uint8_t* buf, int buf_size, bool downmix, bool reset_statistics) {
	if (!ctx) {
		return -1;
	}
	info = get_info(buf, buf_size);
	if (!info) {
		return -2;
	}
	switch (info.stream_type) {
	case STREAM_TYPE_MLP:
		ctx->codec = avcodec_find_decoder(AV_CODEC_ID_MLP);
		break;
	case STREAM_TYPE_TRUEHD:
		ctx->codec = avcodec_find_decoder(AV_CODEC_ID_TRUEHD);
		break;
	default:
		return -3;
	}
	if (ctx->codec) {
		ctx->codecCtx = avcodec_alloc_context3(ctx->codec);
		if (ctx->codecCtx) {
			ctx->codecCtx->max_samples = info.group1_samplerate;
			if (avcodec_open2(ctx->codecCtx, ctx->codec, nullptr) < 0) {
				return -4;
			}
		}
		ctx->parserCtx = av_parser_init(ctx->codec->id);
		if (!ctx->parserCtx) {
			return -5;
		}
	}
	ctx->packet = av_packet_alloc();
	if (!ctx->packet) {
		return -6;
	}
	ctx->frame = av_frame_alloc();
	if (!ctx->frame) {
		return -7;
	}
	if (ctx->codecCtx) {
		ctx->codecCtx->max_samples = (info.group1_channels + info.group2_channels) * ctx->mh.group1_samplerate;
	}
	std::memcpy(&ctx->mlp_mh, buf + info.sync_offset + 4, std::min(buf_size - info.sync_offset - 4, (int)sizeof(ctx->mlp_mh)));
	do_downmix = downmix;
	if (downmix) {
		if (info.can_downmix) {
			av_channel_layout_default(&((mlp_dc_t*)ctx->codecCtx->priv_data)->downmix_layout, 2);
		}
		else {
			set_downmix_coef();
		}
	}
	if (reset_statistics) {
		reset_stats();
	}
	return 0;
}

int mlp_audio_stream_t::decode(uint8_t* data, int* data_size, uint8_t* buf, int buf_size) {
	if (do_check && buf_size >= (int)sizeof(mlp_mh_t) + 4) {
		mlp_mh_t* buf_mh = (mlp_mh_t*)(buf + 4);
		if ((buf_mh->major_sync & 0xfeffffff) == 0xba6f72f8 && (ctx->mlp_mh.major_sync & 0xfeffffff) == 0xba6f72f8) {
			if (
				buf_mh->channel_assignment != ctx->mlp_mh.channel_assignment ||
				buf_mh->group1_samplerate != ctx->mlp_mh.group1_samplerate ||
				buf_mh->group1_bits != ctx->mlp_mh.group1_bits ||
				buf_mh->group2_samplerate != ctx->mlp_mh.group2_samplerate ||
				buf_mh->group2_bits != ctx->mlp_mh.group2_bits
				) {
				return RETCODE_REINIT;
			}
		}
	}
	auto bytes_decoded = av_parser_parse2(
		ctx->parserCtx,
		ctx->codecCtx,
		&ctx->packet->data,
		&ctx->packet->size,
		buf,
		buf_size,
		AV_NOPTS_VALUE,
		AV_NOPTS_VALUE,
		0
	);
	if (ctx->packet->size) {
		auto ret = decode_packet(data, data_size);
		if (ret < 0) {
			return RETCODE_EXCEPT;
		}
	}
	return bytes_decoded;
}

int mlp_audio_stream_t::resync(uint8_t* buf, int buf_size) {
	uint32_t major_sync = 0;
	for (int i = 4; i < buf_size; i++) {
		major_sync = (major_sync << 8) | buf[i];
		if ((major_sync & 0xfffffffe) == 0xf8726fba)
			return i - 7;
	}
	return -1;
}

int mlp_audio_stream_t::decode_packet(uint8_t* data, int* data_size) {
	auto out_data{ data };
	auto out_size{ 0 };
	auto ret = avcodec_send_packet(ctx->codecCtx, ctx->packet);
	while (ret >= 0) {
		if (avcodec_receive_frame(ctx->codecCtx, ctx->frame) < 0) {
			break;
		}
		auto sample_size = av_get_bytes_per_sample(ctx->codecCtx->sample_fmt);
		if (sample_size < 0) {
			break;
		}
		auto frame_size = ctx->frame->nb_samples * ctx->codecCtx->ch_layout.nb_channels * sample_size;
		std::memcpy(out_data, ctx->frame->data[0], frame_size);
		auto buf_bits_read = 8 * ctx->packet->size;
		auto buf_bits_decoded = frame_size / sample_size * ctx->codecCtx->bits_per_raw_sample;
		if (do_downmix && !info.can_downmix) {
			downmix_channels(out_data, &frame_size);
		}
		if (info.can_downmix) {
			buf_bits_decoded = (buf_bits_decoded * (info.group1_channels + info.group2_channels)) / ctx->codecCtx->ch_layout.nb_channels;
		}
		update_stats(buf_bits_read, buf_bits_decoded);
		out_data += frame_size;
		out_size += frame_size;
	}
	*data_size = out_size;
	return ret;
}
