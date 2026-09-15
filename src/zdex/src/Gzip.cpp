#include "zdex/Gzip.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

namespace zircon::zdex {
namespace {

// --- CRC32 ---------------------------------------------------------------------------

std::array<std::uint32_t, 256> BuildCrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    return table;
}

const std::array<std::uint32_t, 256>& CrcTable() {
    static const auto table = BuildCrcTable();
    return table;
}

// --- bit writer ----------------------------------------------------------------------
//
// DEFLATE packs codes LSB-first within a byte, except Huffman codes themselves, which are
// written MSB-first. Getting those two the wrong way round produces a stream that looks
// plausible and decodes to nothing, so they are separate calls here rather than one with
// a flag.

class BitWriter {
public:
    explicit BitWriter(std::string& out) : out_(out) {}

    void Bits(std::uint32_t value, int count) {
        bits_ |= static_cast<std::uint64_t>(value) << held_;
        held_ += count;
        while (held_ >= 8) {
            out_.push_back(static_cast<char>(bits_ & 0xFF));
            bits_ >>= 8;
            held_ -= 8;
        }
    }

    // A Huffman code is stored most-significant-bit first.
    void Code(std::uint32_t code, int length) {
        for (int i = length - 1; i >= 0; --i) Bits((code >> i) & 1, 1);
    }

    void AlignToByte() {
        if (held_ > 0) Bits(0, 8 - held_);
    }

private:
    std::string& out_;
    std::uint64_t bits_{0};
    int held_{0};
};

// --- the fixed Huffman alphabet (RFC 1951 §3.2.6) ------------------------------------
//
// Fixed codes rather than dynamic. Dynamic Huffman would compress a further ~35% on JSON,
// but it means emitting the code-length alphabet correctly as well, and this has to be
// right the first time on a 300 MB upload. Fixed still gets 4-6x on dump JSON, which is
// what the 512 MB limit actually cares about.

struct FixedLit {
    std::uint32_t code;
    int len;
};

FixedLit LiteralCode(int symbol) {
    if (symbol <= 143)  return {static_cast<std::uint32_t>(0x30 + symbol), 8};
    if (symbol <= 255)  return {static_cast<std::uint32_t>(0x190 + symbol - 144), 9};
    if (symbol <= 279)  return {static_cast<std::uint32_t>(symbol - 256), 7};
    return {static_cast<std::uint32_t>(0xC0 + symbol - 280), 8};
}

// length code, extra bits, base length
struct LengthCode { int code; int extra_bits; int base; };

const LengthCode kLengths[] = {
    {257,0,3},{258,0,4},{259,0,5},{260,0,6},{261,0,7},{262,0,8},{263,0,9},{264,0,10},
    {265,1,11},{266,1,13},{267,1,15},{268,1,17},
    {269,2,19},{270,2,23},{271,2,27},{272,2,31},
    {273,3,35},{274,3,43},{275,3,51},{276,3,59},
    {277,4,67},{278,4,83},{279,4,99},{280,4,115},
    {281,5,131},{282,5,163},{283,5,195},{284,5,227},
    {285,0,258},
};

struct DistCode { int code; int extra_bits; int base; };

const DistCode kDistances[] = {
    {0,0,1},{1,0,2},{2,0,3},{3,0,4},{4,1,5},{5,1,7},{6,2,9},{7,2,13},
    {8,3,17},{9,3,25},{10,4,33},{11,4,49},{12,5,65},{13,5,97},
    {14,6,129},{15,6,193},{16,7,257},{17,7,385},{18,8,513},{19,8,769},
    {20,9,1025},{21,9,1537},{22,10,2049},{23,10,3073},
    {24,11,4097},{25,11,6145},{26,12,8193},{27,12,12289},
    {28,13,16385},{29,13,24577},
};

const LengthCode& LengthFor(int length) {
    // 258 is its own code with no extra bits; everything else falls in a range.
    for (int i = static_cast<int>(std::size(kLengths)) - 1; i >= 0; --i)
        if (length >= kLengths[i].base) return kLengths[i];
    return kLengths[0];
}

const DistCode& DistanceFor(int distance) {
    for (int i = static_cast<int>(std::size(kDistances)) - 1; i >= 0; --i)
        if (distance >= kDistances[i].base) return kDistances[i];
    return kDistances[0];
}

// --- LZ77 ----------------------------------------------------------------------------

constexpr int kWindow    = 32768;
constexpr int kMinMatch  = 3;
constexpr int kMaxMatch  = 258;
constexpr int kHashBits  = 16;
constexpr int kHashSize  = 1 << kHashBits;
constexpr int kMaxChain  = 128;     // how far back to look; the ratio/speed knob

inline std::uint32_t Hash3(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0]) << 16 | static_cast<std::uint32_t>(p[1]) << 8 |
            p[2]) * 2654435761u >> (32 - kHashBits);
}

// One deflate block per call, holding the whole buffer. Callers chunk the input so this
// never sees more than a few MB at a time.
void DeflateBlock(BitWriter& bits, const unsigned char* data, std::size_t size,
                  bool final_block) {
    bits.Bits(final_block ? 1 : 0, 1);
    bits.Bits(1, 2);                          // 01 = fixed Huffman

    std::vector<int> head(kHashSize, -1);
    std::vector<int> prev(size, -1);

    const auto emit_literal = [&](unsigned char c) {
        const FixedLit lit = LiteralCode(c);
        bits.Code(lit.code, lit.len);
    };

    std::size_t pos = 0;
    while (pos < size) {
        int best_len = 0;
        int best_dist = 0;

        if (pos + kMinMatch <= size) {
            const std::uint32_t h = Hash3(data + pos);
            int candidate = head[h];
            int chain = kMaxChain;

            while (candidate >= 0 && chain-- > 0) {
                const std::size_t dist = pos - static_cast<std::size_t>(candidate);
                if (dist == 0 || dist > kWindow) break;

                // Cheap reject before the real compare: if the byte that would extend the
                // current best does not match, this candidate cannot beat it.
                if (best_len > 0 &&
                    data[candidate + best_len] != data[pos + best_len]) {
                    candidate = prev[candidate];
                    continue;
                }

                std::size_t len = 0;
                const std::size_t limit = std::min<std::size_t>(kMaxMatch, size - pos);
                while (len < limit && data[candidate + len] == data[pos + len]) ++len;

                if (static_cast<int>(len) > best_len) {
                    best_len = static_cast<int>(len);
                    best_dist = static_cast<int>(dist);
                    if (best_len >= kMaxMatch) break;
                }
                candidate = prev[candidate];
            }
        }

        if (best_len >= kMinMatch) {
            const LengthCode& lc = LengthFor(best_len);
            const FixedLit lit = LiteralCode(lc.code);
            bits.Code(lit.code, lit.len);
            if (lc.extra_bits) bits.Bits(best_len - lc.base, lc.extra_bits);

            const DistCode& dc = DistanceFor(best_dist);
            bits.Code(dc.code, 5);            // distance codes are 5 bits, fixed
            if (dc.extra_bits) bits.Bits(best_dist - dc.base, dc.extra_bits);

            // Every position the match covers still has to go in the hash chains, or the
            // next match loses the history and the ratio collapses.
            for (int i = 0; i < best_len; ++i) {
                if (pos + i + kMinMatch <= size) {
                    const std::uint32_t h = Hash3(data + pos + i);
                    prev[pos + i] = head[h];
                    head[h] = static_cast<int>(pos + i);
                }
            }
            pos += best_len;
        } else {
            if (pos + kMinMatch <= size) {
                const std::uint32_t h = Hash3(data + pos);
                prev[pos] = head[h];
                head[h] = static_cast<int>(pos);
            }
            emit_literal(data[pos]);
            ++pos;
        }
    }

    const FixedLit end = LiteralCode(256);    // end-of-block
    bits.Code(end.code, end.len);
}

void PutU32LE(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void GzipHeader(std::string& out) {
    out.push_back('\x1f');
    out.push_back('\x8b');
    out.push_back('\x08');                    // deflate
    out.push_back('\x00');                    // no flags: no name, no extra, no comment
    PutU32LE(out, 0);                         // mtime 0: keeps output reproducible
    out.push_back('\x00');                    // no extra-compression hint
    out.push_back('\xff');                    // unknown OS
}

} // namespace

std::uint32_t Crc32(std::string_view data, std::uint32_t seed) {
    const auto& table = CrcTable();
    std::uint32_t c = seed ^ 0xFFFFFFFFu;
    for (const char ch : data)
        c = table[(c ^ static_cast<unsigned char>(ch)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// One block per slab, so a 300 MB dump never needs its own weight again in match tables.
// The window does not carry across a boundary, which costs a fraction of a percent.
constexpr std::size_t kSlab = 4u << 20;

std::string GzipCompress(std::string_view input, GzipStats* stats) {
    std::string out;
    out.reserve(input.size() / 3 + 64);
    GzipHeader(out);

    {
        // One writer for the whole stream. Blocks are NOT byte-aligned in DEFLATE: pad
        // between them and the decoder reads the padding as the next block header.
        BitWriter bits(out);
        std::size_t at = 0;
        do {
            const std::size_t take = std::min(kSlab, input.size() - at);
            DeflateBlock(bits, reinterpret_cast<const unsigned char*>(input.data()) + at,
                         take, /*final_block=*/at + take >= input.size());
            at += take;
        } while (at < input.size());
        bits.AlignToByte();                   // only now, once, before the trailer
    }

    PutU32LE(out, Crc32(input));
    PutU32LE(out, static_cast<std::uint32_t>(input.size() & 0xFFFFFFFFu));

    if (stats) {
        stats->raw = input.size();
        stats->compressed = out.size();
    }
    return out;
}

bool GzipFile(std::string_view in_path, std::string_view out_path, std::string& error,
              GzipStats* stats, const std::function<bool(std::uint64_t)>& progress) {
    std::FILE* in = nullptr;
    if (::fopen_s(&in, std::string(in_path).c_str(), "rb") != 0 || !in) {
        error = "cannot open '" + std::string(in_path) + "'";
        return false;
    }
    std::FILE* out = nullptr;
    if (::fopen_s(&out, std::string(out_path).c_str(), "wb") != 0 || !out) {
        std::fclose(in);
        error = "cannot write '" + std::string(out_path) + "'";
        return false;
    }

    std::string header;
    GzipHeader(header);
    std::fwrite(header.data(), 1, header.size(), out);

    std::string slab(kSlab, '\0');
    std::uint64_t raw = 0;
    std::uint64_t written = header.size();
    std::uint32_t crc = 0;
    bool aborted = false;

    // One writer across every block. Its sink is drained to disk after each slab, but the
    // writer itself survives, because the up-to-seven bits it is still holding belong to
    // the next block. Padding them out would be read back as a block header.
    std::string sink;
    BitWriter bits(sink);

    for (;;) {
        const std::size_t got = std::fread(slab.data(), 1, kSlab, in);
        const bool last = got < kSlab || std::feof(in);

        DeflateBlock(bits, reinterpret_cast<const unsigned char*>(slab.data()), got, last);
        if (last) bits.AlignToByte();

        crc = Crc32(std::string_view(slab.data(), got), crc);
        raw += got;

        std::fwrite(sink.data(), 1, sink.size(), out);
        written += sink.size();
        sink.clear();

        if (progress && !progress(raw)) { aborted = true; break; }
        if (last) break;
    }

    std::fclose(in);

    if (!aborted) {
        std::string trailer;
        PutU32LE(trailer, crc);
        PutU32LE(trailer, static_cast<std::uint32_t>(raw & 0xFFFFFFFFu));
        std::fwrite(trailer.data(), 1, trailer.size(), out);
        written += trailer.size();
    }
    std::fclose(out);

    if (aborted) {
        std::remove(std::string(out_path).c_str());
        error = "cancelled";
        return false;
    }

    if (stats) {
        stats->raw = raw;
        stats->compressed = written;
    }
    return true;
}

} // namespace zircon::zdex
