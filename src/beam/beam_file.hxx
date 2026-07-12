#pragma once
// BEAM file (IFF) reader and writer. Reads a .beam file into a list of
// named chunks, allows adding/replacing chunks, and writes back a valid
// IFF/BEAM container. Standard OTP chunks are preserved byte-for-byte;
// the KexI custom chunk is the only one Kex writes.
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace kex::beam {

struct BeamError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Chunk {
    std::string id;              // 4-byte ASCII chunk ID
    std::vector<uint8_t> data;   // raw payload (unpadded)
};

struct BeamFile {
    std::vector<Chunk> chunks;

    auto findChunk(const std::string& id) const -> const Chunk*;
    auto findChunk(const std::string& id) -> Chunk*;
    void setChunk(const std::string& id, std::vector<uint8_t> data);
    void removeChunk(const std::string& id);
};

auto readBeamFile(const std::string& path) -> BeamFile;
auto readBeamFile(const std::vector<uint8_t>& bytes) -> BeamFile;
auto writeBeamFile(const BeamFile& bf) -> std::vector<uint8_t>;
void writeBeamFile(const BeamFile& bf, const std::string& path);

} // namespace kex::beam
