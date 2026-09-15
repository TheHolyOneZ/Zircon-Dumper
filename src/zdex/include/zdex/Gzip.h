#pragma once

// A gzip writer, because Zdex takes a 300 MB dump far better as 20 MB and nothing in this
// project had compression in it. The one zip we already write (ReClass) stores its entries
// uncompressed and gets away with it; an upload cannot.
//
// Written rather than vendored. zlib would be a third dependency for one function, and the
// correctness risk of writing DEFLATE is answerable: every unit test here decompresses the
// output with Python's zlib, which is an entirely separate implementation, and compares it
// to the input byte for byte.

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

// Exposed for the tests. CRC32 as gzip and zip both define it.
std::uint32_t Crc32(std::string_view data, std::uint32_t seed = 0);

} // namespace zircon::zdex
