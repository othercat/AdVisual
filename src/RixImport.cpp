#include <RixImport.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

namespace RixImport {
namespace {
struct Event { int ms, command, channel, value; };
unsigned word(std::span<const uint8_t> data, size_t pos) {
    return data[pos] | (unsigned(data[pos + 1]) << 8);
}

OPLFM read_operator(std::span<const uint8_t> data, size_t pos, unsigned wave) {
    auto v = [&](unsigned i) { return word(data, pos + i * 2); };
    OPLFM op;
    op.ksl = v(0) & 3;
    op.frequency_multiplier = v(1) & 15;
    op.feedback = v(2) & 7;
    op.attack_rate = v(3) & 15;
    op.sustain_level = v(4) & 15;
    op.sustain_sound = v(5) != 0;
    op.decay_rate = v(6) & 15;
    op.release_rate = v(7) & 15;
    op.output_level = v(8) & 63;
    op.tremelo = v(9) != 0;
    op.vibrato = v(10) != 0;
    op.ksr = v(11) != 0;
    op.additive_synth = v(12) == 0; // INS connection flag is inverted.
    op.waveform = wave & 3;
    return op;
}

std::string instrument_name(int index, int channel) {
    char name[9];
    std::snprintf(name, sizeof(name), "RIX%03d%02d", index, channel);
    return name;
}
}

int read(std::span<const uint8_t> data, Track& track, Bank& bank) {
    if (data.size() < 22 || word(data, 0) != 0x55aa || data[2] > 1)
        throw std::runtime_error("Not a supported RIX file (expected 55AA header).");
    const unsigned ins = word(data, 8), pattern = word(data, 12);
    if (ins < 20 || pattern < ins || pattern + 1 >= data.size() || (pattern - ins) % 64)
        throw std::runtime_error("Invalid RIX instrument/pattern offsets.");
    const bool rhythm = data[2] != 0;
    const int channel_count = rhythm ? 11 : 9;
    int duration = 0;
    bool eof = false;
    std::vector<Event> events;
    std::set<std::pair<int, int>> patches;
    for (size_t pos = pattern; pos + 1 < data.size(); pos += 2) {
        const unsigned value = word(data, pos);
        if (value == 0x8000) { eof = true; break; }
        int command = value >> 12, channel = (value >> 8) & 15, low = value & 255;
        if (command >= 9 && command <= 12) {
            if (channel >= channel_count)
                throw std::runtime_error("RIX command uses an unavailable channel.");
            if (command == 9) {
                if (ins + low * 64 + 56 > pattern)
                    throw std::runtime_error("RIX instrument is outside its bank.");
                patches.emplace(low, channel);
            }
            events.push_back({duration, command, channel, low});
        } else {
            if (duration > std::numeric_limits<int>::max() - int(value))
                throw std::runtime_error("RIX duration is too long.");
            duration += value;
        }
    }
    if (!eof) throw std::runtime_error("RIX is truncated (missing 8000 end marker).");
    int resolution = 0;
    for (int candidate : {1, 2, 4, 5, 10, 20, 25, 50, 100, 125, 250, 500})
        if ((duration + candidate - 1) / candidate <= 65535) { resolution = candidate; break; }
    if (!resolution) throw std::runtime_error("RIX exceeds the ROL editor's timing range.");
    auto to_tick = [resolution](int ms) { return int(std::lround(double(ms) / resolution)); };

    std::vector<Channel> channels;
    for (int ch = 0; ch < 11; ++ch) {
        channels.emplace_back(ch);
        channels.back().instrument_events.clear();
        channels.back().volume_events = {{0, 1.0f}};
        channels.back().pitch_events = {{0, 1.0f}};
    }
    std::vector<Instrument> instruments;
    for (auto [index, ch] : patches) {
        const size_t base = ins + index * 64;
        Instrument instrument;
        const auto name = instrument_name(index, ch);
        std::copy(name.begin(), name.end(), instrument.name);
        instrument.flags = 1;
        instrument.percussion_mode = rhythm && ch >= 6;
        instrument.voice_number = instrument.percussion_mode ? ch : 0;
        instrument.modulator = read_operator(data, base, word(data, base + 52));
        instrument.carrier = read_operator(data, base + 26, word(data, base + 54));
        instruments.push_back(instrument);
    }
    std::sort(instruments.begin(), instruments.end(), [](const auto& a, const auto& b) {
        return std::string(a.name) < std::string(b.name);
    });
    std::array<int, 11> active_note{}, note_start{};
    auto close_note = [&](int ch, int tick) {
        if (active_note[ch] && tick > note_start[ch])
            channels[ch].notes.push_back(Note(note_start[ch], 107 - std::clamp(active_note[ch], 12, 107), tick - note_start[ch]));
    };
    for (const auto& event : events) {
        auto& channel = channels[event.channel];
        const int tick = to_tick(event.ms);
        switch (event.command) {
        case 9: channel.instrument_events[tick] = instrument_name(event.value, event.channel); break;
        // Match rixrol's neutral=128 pitch mapping; this is not a raw OPL
        // register editor and ROL's pitch-bend quantization still applies.
        case 10: channel.pitch_events[tick] = event.value / 128.0f; break;
        // RIX uses 127 as full volume. Preserve that exactly (not 127/128).
        case 11: channel.volume_events[tick] = std::min(event.value, 127) / 127.0f; break;
        case 12:
            close_note(event.channel, tick);
            active_note[event.channel] = event.value;
            note_start[event.channel] = tick;
            break;
        }
    }
    for (int ch = 0; ch < 11; ++ch) close_note(ch, to_tick(duration));

    // Publish only after all validation and conversion succeeded.
    track.clear_track_data();
    track.channels = std::move(channels);
    track.minimum_tick_count = to_tick(duration);
    track.ticks_per_beat = 500 / resolution;
    track.beats_per_measure = 4;
    track.basic_tempo = 120;
    track.melodic_mode = !rhythm;
    track.tempo_events = {{0, 1.0f}};
    track.meta_data = "PAL RIX import: " + std::to_string(resolution) + " ms grid";
    track.file_path.clear();
    bank.instruments = std::move(instruments);
    bank.file_path.clear();
    return resolution;
}

void load_file(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open RIX file.");
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
    const int resolution = read(data, *current_track, *current_bank);
    current_channel = current_track->get_channel(0);
    enabled_channels.fill(true);
    signal_bank_changed.emit();
    signal_track_changed.emit();
    std::cout << "Imported RIX on a " << resolution << " ms grid; save as a new ROL + BNK pair.\n";
}
}
