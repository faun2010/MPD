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

#pragma once

#include <cstdint>
#include <thread>
#include <vector>
#include <std_semaphore.h>

using dsx_buffer_t = std::vector<uint8_t>;

constexpr auto LOG_ERROR   { "Error: " };
constexpr auto LOG_WARNING { "Warning: " };
constexpr auto LOG_INFO    { "Info: " };

template<typename T>
static void LOG(T p1, T p2) {
	log_printf(T("%s%s"), p1, p2);
}

enum class model_e { MT = 1, MPP = 2 };
enum class slot_state_t { SLOT_EMPTY, SLOT_LOADED, SLOT_RUNNING, SLOT_READY, SLOT_READY_WITH_ERROR, SLOT_TERMINATING };

template<typename codec_t>
class dst_slot_t {
public:
	std::thread thread;
	semaphore_t inp_semaphore;
	semaphore_t out_semaphore;
	codec_t     codec;

	slot_state_t state;
	dsx_buffer_t inp_data;
	dsx_buffer_t out_data;

	dst_slot_t() : inp_semaphore(0), out_semaphore(0) {
		state = slot_state_t::SLOT_EMPTY;
	}
	dst_slot_t(const dst_slot_t& slot) = delete;
	dst_slot_t& operator=(const dst_slot_t& slot) = delete;
	dst_slot_t(dst_slot_t&& slot) : inp_semaphore(0), out_semaphore(0) {
		state = slot.state;
		inp_data = std::move(slot.inp_data);
		out_data = std::move(slot.out_data);
	}
	dst_slot_t& operator=(dst_slot_t&& slot) = delete;
	void run(bool& running) {
		while (running) {
			inp_semaphore.acquire();
			if (running && !inp_data.empty()) {
				state = slot_state_t::SLOT_RUNNING;
				auto error = codec.run(inp_data.data(), (unsigned int)(8 * inp_data.size()), out_data.data());
				state = (error == 0) ? slot_state_t::SLOT_READY : slot_state_t::SLOT_READY_WITH_ERROR;
			}
			else {
				out_data.clear();
			}
			out_semaphore.release();
		}
	}
};

template<typename codec_t, model_e model>
class dst_engine_t {
	std::vector<dst_slot_t<codec_t>> slots;

	size_t slot_index;
	size_t out_size;
	bool   run_threads;
public:
	dst_engine_t(size_t num_threads = 0u) {
		slot_index = 0;
		out_size = 0;
		run_threads = true;
		slots.resize(num_threads ? num_threads : std::thread::hardware_concurrency());
		for (auto&& slot : slots) {
			std::thread t([this, &slot]() { slot.run(run_threads); });
			if (!t.joinable()) {
				LOG(LOG_ERROR, "Could not start DST decoder thread");
				return;
			}
			slot.thread = std::move(t);
		}
	}
	
	~dst_engine_t() {
		run_threads = false;
		for (auto&& slot : slots) {
			slot.state = slot_state_t::SLOT_TERMINATING;
			slot.inp_semaphore.release(); // Release worker (decoding) thread for exit
			slot.thread.join(); // Wait until worker (decoding) thread exit
			slot.codec.close();
		}
	}
	
	template<typename T1, typename T2, typename... Args>
	auto init(T1 channels, T2 frame_size, Args&&... args) {
		auto rv{ 0 };
		out_size = static_cast<decltype(out_size)>(channels) * static_cast<decltype(out_size)>(frame_size);
		for (auto&& slot : slots) {
			rv = slot.codec.init(channels, frame_size, std::forward<Args>(args)...);
			if (rv) {
				return rv;
			}
		}
		return rv;
	}
	
	int run(dsx_buffer_t& dsx_data) {

		// Get current slot
		auto&& slot_set = slots[slot_index];

		// Allocate encoded frame into the slot
		slot_set.inp_data = std::move(dsx_data);
		slot_set.out_data.resize(out_size);

		// Release worker (decoding) thread on the loaded slot
		if (!slot_set.inp_data.empty()) {
			slot_set.state = slot_state_t::SLOT_LOADED;
			if constexpr(model == model_e::MT) {
				slot_set.inp_semaphore.release();
			}
		}
		else {
			slot_set.state = slot_state_t::SLOT_EMPTY;
		}

		if constexpr(model == model_e::MPP) {
			// Release all worker (decoding) threads
			if (slot_index == slots.size() - 1) {
				for (auto& slot : slots) {
					slot.inp_semaphore.release();
				}
			}
		}

		// Move to the oldest slot
		slot_index = (slot_index + 1) % slots.size();
		auto&& slot_get = slots[slot_index];

		// Save decoded frame
		if (slot_get.state != slot_state_t::SLOT_EMPTY) {
			slot_get.out_semaphore.acquire();
		}
		switch (slot_get.state) {
		case slot_state_t::SLOT_READY:
		case slot_state_t::SLOT_READY_WITH_ERROR:
			dsx_data = std::move(slot_get.out_data);
			break;
		default:
			dsx_data.clear();
			break;
		}
		return (int)dsx_data.size();
	}

	void flush() {
		for (auto& slot : slots) {
			(void)slot;
			dsx_buffer_t dsx_temp;
			run(dsx_temp);
		}
	}

};
