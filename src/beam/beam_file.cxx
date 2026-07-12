#include "beam_file.hxx"
#include <algorithm>
#include <cstring>
#include <fstream>

namespace kex::beam {

namespace {

auto readU32(const uint8_t* p) -> uint32_t {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void writeU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

auto alignUp(size_t n) -> size_t {
    return (n + 3) & ~size_t(3);
}

} // namespace

auto BeamFile::findChunk(const std::string& id) const -> const Chunk* {
    for (const auto& c : chunks)
        if (c.id == id) return &c;
    return nullptr;
}

auto BeamFile::findChunk(const std::string& id) -> Chunk* {
    for (auto& c : chunks)
        if (c.id == id) return &c;
    return nullptr;
}

void BeamFile::setChunk(const std::string& id, std::vector<uint8_t> data) {
    if (auto* c = findChunk(id)) {
        c->data = std::move(data);
    } else {
        chunks.push_back({id, std::move(data)});
    }
}

void BeamFile::removeChunk(const std::string& id) {
    chunks.erase(
        std::remove_if(chunks.begin(), chunks.end(),
                        [&](const Chunk& c) { return c.id == id; }),
        chunks.end());
}

auto readBeamFile(const std::vector<uint8_t>& bytes) -> BeamFile {
    if (bytes.size() < 12)
        throw BeamError("file too small to be a BEAM file");
    if (std::memcmp(bytes.data(), "FOR1", 4) != 0)
        throw BeamError("not an IFF file (missing FOR1 header)");
    uint32_t formSize = readU32(bytes.data() + 4);
    if (formSize + 8 > bytes.size())
        throw BeamError("IFF form size exceeds file size");
    if (std::memcmp(bytes.data() + 8, "BEAM", 4) != 0)
        throw BeamError("not a BEAM file (missing BEAM form type)");

    BeamFile bf;
    size_t pos = 12;
    size_t end = 8 + formSize;
    while (pos + 8 <= end) {
        std::string id(reinterpret_cast<const char*>(bytes.data() + pos), 4);
        uint32_t chunkSize = readU32(bytes.data() + pos + 4);
        pos += 8;
        if (pos + chunkSize > end)
            throw BeamError("chunk '" + id + "' extends past end of file");
        std::vector<uint8_t> data(bytes.data() + pos,
                                  bytes.data() + pos + chunkSize);
        bf.chunks.push_back({std::move(id), std::move(data)});
        pos += alignUp(chunkSize);
    }
    return bf;
}

auto readBeamFile(const std::string& path) -> BeamFile {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw BeamError("cannot open: " + path);
    std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());
    return readBeamFile(bytes);
}

auto writeBeamFile(const BeamFile& bf) -> std::vector<uint8_t> {
    // Compute total size of all chunks (each: 4 id + 4 size + aligned data).
    size_t bodySize = 4; // "BEAM"
    for (const auto& c : bf.chunks)
        bodySize += 8 + alignUp(c.data.size());

    std::vector<uint8_t> out;
    out.reserve(8 + bodySize);

    // FOR1 header
    out.insert(out.end(), {'F', 'O', 'R', '1'});
    writeU32(out, static_cast<uint32_t>(bodySize));
    out.insert(out.end(), {'B', 'E', 'A', 'M'});

    for (const auto& c : bf.chunks) {
        out.insert(out.end(), c.id.begin(), c.id.end());
        writeU32(out, static_cast<uint32_t>(c.data.size()));
        out.insert(out.end(), c.data.begin(), c.data.end());
        size_t padding = alignUp(c.data.size()) - c.data.size();
        for (size_t i = 0; i < padding; i++)
            out.push_back(0);
    }
    return out;
}

void writeBeamFile(const BeamFile& bf, const std::string& path) {
    auto bytes = writeBeamFile(bf);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        throw BeamError("cannot write: " + path);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

} // namespace kex::beam
