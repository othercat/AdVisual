#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <Track.h>
#include <Instrument.h>

namespace RixImport {
// Throws on invalid input, leaving the existing song and bank untouched.
// ROL uses 16-bit ticks: use the finest 1/2/4/... ms grid that fits the song.
int read(std::span<const uint8_t> data, Track& track, Bank& bank);
void load_file(const std::string& filename);
}
