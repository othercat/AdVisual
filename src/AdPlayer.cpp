#include <AdPlayer.h>
#include <algorithm>
#include <iostream>
#include <common.h>
#include <ComposerPanel.h>

namespace {
constexpr int rate = 44100;
constexpr unsigned int output_channels = 2;
}

unique_ptr<AdPlayer> adplayer = nullptr;

// All offsets are frames. Nuked writes two interleaved samples per frame.
int AdPlayer::adplug_process(short* buffer, unsigned int frames, unsigned int offset) {
	unsigned int written = 0;
	while (written < frames && active) {
		if (towrite <= 0 && playing_song) {
			const int tick = opl_playback->getsubsong();
			if (tick >= range_end) {
				if (looping && range_end > range_start) {
					opl_playback->rewind(range_start);
					apply_channel_states();
				}
				else {
					playback_tick = range_end;
					active = false;
					finished = true;
					break;
				}
			}
			if (!opl_playback->update()) {
				active = false;
				finished = true;
				break;
			}
			playback_tick = std::max(range_start, int(opl_playback->getsubsong()) - 1);
			const double samples = rate / double(std::max(0.01f, opl_playback->getrefresh())) + tick_fraction;
			towrite = std::max(1L, long(samples));
			tick_fraction = samples - towrite;
		}
		if (!playing_song) towrite = frames - written;
		const unsigned int count = std::min<unsigned int>(towrite, frames - written);
		opl_device->update(buffer + (offset + written) * output_channels, count);
		towrite -= count;
		written += count;
	}
	return written;
}

void AdPlayer::mix_miniaudio(ma_device*, void* output, const void*, ma_uint32 frame_count) {
	auto* buffer = static_cast<short*>(output);
	std::fill_n(buffer, frame_count * output_channels, short(0));
	std::lock_guard<std::mutex> lock(playback_mutex);
	if (active && opl_playback) adplug_process(buffer, frame_count, 0);
	// No GTK calls, signals, detached threads, or device stop in this callback.
}

void mix_miniaudio_callback(ma_device* device, void* output, const void* input, ma_uint32 frames) {
	static_cast<AdPlayer*>(device->pUserData)->mix_miniaudio(device, output, input, frames);
}

AdPlayer::AdPlayer() {
	opl_device = make_unique<CNemuopl>(rate);
	opl_device->init(); // OPL2 compatibility mode, without stereo detuning.
	opl_playback.reset(static_cast<CVisPlayer*>(CVisPlayer::factory(opl_device.get(), current_track, current_bank)));
	if (!opl_playback) return;
	opl_playback->rewind(0);
	opl_playback->disable_all_channels();
	ma_device_config config = ma_device_config_init(ma_device_type_playback);
	config.playback.format = ma_format_s16;
	config.playback.channels = output_channels;
	config.sampleRate = rate;
	config.dataCallback = mix_miniaudio_callback;
	config.pUserData = this;
	device_initialized = ma_device_init(nullptr, &config, &mini_device) == MA_SUCCESS;
	if (!device_initialized) cerr << "Failed to initialize miniaudio device!\n";
	playback_timer = Glib::signal_timeout().connect(sigc::mem_fun(*this, &AdPlayer::poll_playback), 33);
}

AdPlayer::~AdPlayer() {
	playback_timer.disconnect();
	stop();
	if (device_initialized) ma_device_uninit(&mini_device);
}

bool AdPlayer::poll_playback() {
	if (playing_song && playback_tick != cursor_tick) ComposerPanel::set_cursor_tick(playback_tick);
	if (finished.exchange(false)) stop();
	return true;
}

void AdPlayer::toggle_loop_mode() {
	looping = !looping.load();
	if (common_action_group) common_action_group->change_action_state("toggle_loop_mode", Glib::Variant<bool>::create(looping.load()));
}

void AdPlayer::play_note(int note, int channel, Instrument* instrument, float pitch, float volume) {
	if (!device_initialized || !opl_playback || playing_song) return;
	{
		std::lock_guard<std::mutex> lock(playback_mutex);
		if (instrument && instrument->percussion_mode) channel = instrument->voice_number;
		opl_playback->set_rhythm_mode((instrument && instrument->percussion_mode) || !current_track->melodic_mode);
		opl_playback->channel_play_note(channel, note, instrument, pitch, volume);
		active = true;
	}
	ma_device_start(&mini_device);
}

void AdPlayer::apply_channel_states() {
	for (int channel = 0; channel < (current_track->melodic_mode ? 9 : 11); ++channel) {
		if (enabled_channels[channel]) opl_playback->enable_channel(channel);
		else opl_playback->channel_play_note(channel, -1, nullptr);
	}
}

void AdPlayer::set_channel_enable(int channel, bool enable) {
	if (channel < 0 || channel >= 11) return;
	std::lock_guard<std::mutex> lock(playback_mutex);
	enabled_channels[channel] = enable;
	if (!opl_playback) return;
	if (current_track->melodic_mode && channel >= 9) return;
	if (enable && playing_song) opl_playback->enable_channel(channel);
	else opl_playback->channel_play_note(channel, -1, nullptr);
}

bool AdPlayer::play(string, int start_from) {
	if (!device_initialized || !opl_playback) return false;
	ma_device_stop(&mini_device); // Wait for the callback before changing playback state.
	{
		std::lock_guard<std::mutex> lock(playback_mutex);
		opl_playback->set_track(current_track);
		opl_playback->set_bank(current_bank);
		range_start = ComposerPanel::has_selection() ? ComposerPanel::selection_start() : 0;
		range_end = ComposerPanel::has_selection() ? ComposerPanel::selection_end() : current_track->get_tick_count();
		if (range_end <= range_start) return false;
		start_from = std::clamp(start_from, range_start, range_end - 1);
		opl_playback->rewind(start_from);
		playing_song = true;
		apply_channel_states();
		towrite = 0;
		tick_fraction = 0;
		playback_tick = start_from;
		finished = false;
		active = true;
	}
	if (ma_device_start(&mini_device) != MA_SUCCESS) { stop(); return false; }
	if (common_action_group) common_action_group->change_action_state("playing_song", Glib::Variant<bool>::create(true));
	ComposerPanel::signal_playback_changed.emit();
	return true;
}

void AdPlayer::seek(unsigned long tick) {
	if (!playing_song) { ComposerPanel::set_cursor_tick(std::min<unsigned long>(tick, 65535)); return; }
	std::lock_guard<std::mutex> lock(playback_mutex);
	const int destination = std::clamp<int>(std::min<unsigned long>(tick, 65535), range_start, range_end - 1);
	opl_playback->rewind(destination);
	apply_channel_states();
	towrite = 0;
	tick_fraction = 0;
	playback_tick = destination;
}

void AdPlayer::stop() {
	if (device_initialized) ma_device_stop(&mini_device);
	{
		std::lock_guard<std::mutex> lock(playback_mutex);
		if (opl_playback) opl_playback->disable_all_channels();
		towrite = 0;
		active = false;
		playing_song = false;
		finished = false;
	}
	if (common_action_group) common_action_group->change_action_state("playing_song", Glib::Variant<bool>::create(false));
	ComposerPanel::signal_playback_changed.emit();
}
