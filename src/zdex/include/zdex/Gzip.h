#pragma once

// Gzip, both ways. Zdex takes a 300 MB dump far better as 20 MB, and once dumps are written
// compressed every command that reads one has to be able to open it -- 0.7.0 shipped the
// writer without the reader and four commands could not read what the batch runner wrote.
//
// Written rather than vendored. zlib would be a third dependency, and the correctness risk is
// answerable both ways: the writer's tests decompress with Python's zlib, and the reader's
// tests inflate what Python's gzip produced, including the dynamic-Huffman blocks this
// compressor never emits.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace zircon::zdex {

struct GzipStats {
    std::uint64_t raw{0};
    std::uint64_t compressed{0};

    double ratio() const {
        return compressed == 0 ? 0.0 : static_cast<double>(raw) / static_cast<double>(compressed);
    }
};

// Compresses `input` into a complete gzip member (header, deflate stream, CRC32, length).
// The result is what `gzip -c` would produce, not a raw deflate stream: Zdex wants a file
// ending .json.gz and so does anything else that might open it.
std::string GzipCompress(std::string_view input, GzipStats* stats = nullptr);

// Streams a file through the compressor instead of holding two copies of a 300 MB dump in
// memory. `progress` is called with bytes read so far; return false from it to abort.
bool GzipFile(std::string_view in_path, std::string_view out_path, std::string& error,
              GzipStats* stats = nullptr,
              const std::function<bool(std::uint64_t)>& progress = {});

// The two-byte magic. Cheap enough to call on anything before deciding how to read it.
bool LooksGzipped(std::string_view data);

// Inflates a complete gzip member. Empty with `error` set when the input is not gzip, is
// truncated, or fails its own CRC32 -- a dump that inflates to something the file itself
// says is wrong is not a dump worth parsing.
//
// Handles all three DEFLATE block types, not only the fixed-Huffman ones GzipCompress emits:
// a file gzipped by anything else arrives here too.
std::string GzipDecompress(std::string_view input, std::string& error);

// Whole file in, whole file out. Same refusals.
bool GunzipFile(std::string_view in_path, std::string_view out_path, std::string& error);

// Exposed for the tests. CRC32 as gzip and zip both define it.
std::uint32_t Crc32(std::string_view data, std::uint32_t seed = 0);

} // namespace zircon::zdex
