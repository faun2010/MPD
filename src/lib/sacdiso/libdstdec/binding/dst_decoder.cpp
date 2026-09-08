/*
* SACD Decoder plugin
* Copyright (c) 2011-2023 Maxim V.Anisiutkin <maxim.anisiutkin@gmail.com>
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with FFmpeg; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "dst_decoder.h"
#include "decoder.h"
#include "dst_engine.h"

class dst_decoder_t::ctx_t : public dst_engine_t<dst::decoder_t, model_e::MT> {
};

dst_decoder_t::dst_decoder_t() : ctx(nullptr) {
}

dst_decoder_t::~dst_decoder_t() {
	delete ctx;
}

int dst_decoder_t::init(unsigned int channels, unsigned int channel_frame_size) {
	if (!ctx) {
		ctx = new ctx_t();
	}
	if (!ctx) {
		return -1;
	}
	return ctx->init(channels, channel_frame_size);
}

int dst_decoder_t::run(std::vector<unsigned char>& dsx_data) {
	if (!ctx) {
		return 0;
	}
	return ctx->run(dsx_data);
}

void dst_decoder_t::flush() {
	if (!ctx) {
		return;
	}
	return ctx->flush();
}
