#pragma once
#include <miniaudio.h>
#include <adplug/adplug.h>
#include <adplug/emuopl.h>
#include <adplug/player.h>
#include <adplug/nemuopl.h> // Nuked OPL.
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <Instrument.h>
#include <VisPlayer.h>

using namespace std;

class AdPlayer {
public:
	AdPlayer();
	~AdPlayer();
	void toggle_loop_mode();
	bool play(string file_path = "", int start_from = 0); // Returns false if failed to play.
	void play_note(int note_number, int channel, Instrument* instrument, float pitch = 1.0, float volume = 1.0);
	void stop();
	void seek(unsigned long p_tick);
	void mix_miniaudio(ma_device* p_device, void* p_output, const void* p_input, ma_uint32 frame_count);
	void set_channel_enable(int channel, bool enable);
	bool get_channel_enable(int channel) const { return enabled_channels[channel]; }
	bool is_playing() const { return playing_song; }
protected:
	bool poll_playback();
	void apply_channel_states();
	long towrite = 0;
	double tick_fraction = 0;
	ma_device mini_device{};
	unique_ptr<CVisPlayer> opl_playback;
	unique_ptr<Copl> opl_device;
	std::atomic<bool> looping{false}, active{false}, playing_song{false}, finished{false};
	std::atomic<int> playback_tick{0};
	std::mutex playback_mutex;
	sigc::connection playback_timer;
	bool device_initialized = false;
	int range_start = 0, range_end = 0;
	int adplug_process(short* p_buffer, unsigned int p_frames, unsigned int p_buffer_offset);
	friend class ma_device;
};

extern unique_ptr<AdPlayer> adplayer;
