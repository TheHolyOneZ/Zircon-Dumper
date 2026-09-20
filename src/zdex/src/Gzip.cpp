#include "zdex/Gzip.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
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


// ===================================================================================
// Reading it back
// ===================================================================================

namespace {

// Canonical Huffman, decoded the way RFC 1951 section 3.2.2 describes: count the codes of
// each length, turn that into a first-code-per-length, and walk bit by bit. Slower than a
// lookup table and much easier to be sure of; a 600 MB dump still inflates in a couple of
// seconds because the inner loop is tiny.
class Huffman {
public:
    bool Build(const std::uint8_t* lengths, int count) {
        counts_.assign(kMaxBits + 1, 0);
        symbols_.assign(static_cast<std::size_t>(count), 0);

        for (int i = 0; i < count; ++i) {
            if (lengths[i] > kMaxBits) return false;
            ++counts_[lengths[i]];
        }
        if (counts_[0] == count) return true;       // no codes at all is legal for distances

        // Over-subscribed or incomplete sets are corrupt input, not something to limp past.
        int left = 1;
        for (int bits = 1; bits <= kMaxBits; ++bits) {
            left <<= 1;
            left -= counts_[bits];
            if (left < 0) return false;
        }

        std::vector<int> offsets(kMaxBits + 1, 0);
        for (int bits = 1; bits < kMaxBits; ++bits)
            offsets[bits + 1] = offsets[bits] + counts_[bits];
        for (int i = 0; i < count; ++i)
            if (lengths[i]) symbols_[static_cast<std::size_t>(offsets[lengths[i]]++)] = i;
        return true;
    }

    const std::vector<int>& counts() const { return counts_; }
    const std::vector<int>& symbols() const { return symbols_; }

private:
    static constexpr int kMaxBits = 15;
    std::vector<int> counts_;
    std::vector<int> symbols_;
};

class BitReader {
public:
    explicit BitReader(std::string_view data) : data_(data) {}

    // -1 on running off the end, which every caller checks.
    int Bits(int need) {
        while (held_ < need) {
            if (at_ >= data_.size()) return -1;
            value_ |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data_[at_++])) << held_;
            held_ += 8;
        }
        const int out = static_cast<int>(value_ & ((1u << need) - 1));
        value_ >>= need;
        held_ -= need;
        return out;
    }

    int Decode(const Huffman& table) {
        int code = 0, first = 0, index = 0;
        for (int length = 1; length <= 15; ++length) {
            const int bit = Bits(1);
            if (bit < 0) return -1;
            code |= bit;
            const int count = table.counts()[length];
            if (code - first < count)
                return table.symbols()[static_cast<std::size_t>(index + (code - first))];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return -1;
    }

    void AlignToByte() { value_ = 0; held_ = 0; }
    std::size_t at() const { return at_; }
    void Seek(std::size_t at) { at_ = at; AlignToByte(); }
    bool Exhausted() const { return at_ >= data_.size(); }

private:
    std::string_view data_;
    std::size_t      at_{0};
    std::uint32_t    value_{0};
    int              held_{0};
};

// RFC 1951 section 3.2.5. Kept as data rather than arithmetic because the tables are what the
// spec is; deriving them would be a second thing to get wrong.
constexpr std::uint16_t kLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t kLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t kDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t kDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void BuildFixedTables(Huffman& literals, Huffman& distances) {
    std::uint8_t lengths[288];
    for (int i = 0; i < 144; ++i) lengths[i] = 8;
    for (int i = 144; i < 256; ++i) lengths[i] = 9;
    for (int i = 256; i < 280; ++i) lengths[i] = 7;
    for (int i = 280; i < 288; ++i) lengths[i] = 8;
    literals.Build(lengths, 288);

    std::uint8_t dist[30];
    for (int i = 0; i < 30; ++i) dist[i] = 5;
    distances.Build(dist, 30);
}

bool InflateBlockBody(BitReader& bits, const Huffman& literals, const Huffman& distances,
                      std::string& out, std::string& error) {
    for (;;) {
        const int symbol = bits.Decode(literals);
        if (symbol < 0) {
            error = "the compressed stream ends in the middle of a block";
            return false;
        }
        if (symbol < 256) {
            out.push_back(static_cast<char>(symbol));
            continue;
        }
        if (symbol == 256) return true;                 // end of block

        const int length_code = symbol - 257;
        if (length_code >= 29) {
            error = "invalid length code in the compressed stream";
            return false;
        }
        const int extra = bits.Bits(kLengthExtra[length_code]);
        if (extra < 0) {
            error = "the compressed stream ends in the middle of a length";
            return false;
        }
        const std::size_t length = kLengthBase[length_code] + static_cast<std::size_t>(extra);

        const int dist_code = bits.Decode(distances);
        if (dist_code < 0 || dist_code >= 30) {
            error = "invalid distance code in the compressed stream";
            return false;
        }
        const int dist_extra = bits.Bits(kDistExtra[dist_code]);
        if (dist_extra < 0) {
            error = "the compressed stream ends in the middle of a distance";
            return false;
        }
        const std::size_t distance = kDistBase[dist_code] + static_cast<std::size_t>(dist_extra);
        if (distance > out.size()) {
            error = "the compressed stream refers back further than it has written";
            return false;
        }

        // Byte at a time on purpose: runs may overlap themselves, which is how DEFLATE
        // encodes a repeat, and memcpy would be wrong for exactly those.
        const std::size_t from = out.size() - distance;
        for (std::size_t i = 0; i < length; ++i) out.push_back(out[from + i]);
    }
}

bool InflateDynamic(BitReader& bits, std::string& out, std::string& error) {
    const int hlit  = bits.Bits(5);
    const int hdist = bits.Bits(5);
    const int hclen = bits.Bits(4);
    if (hlit < 0 || hdist < 0 || hclen < 0) {
        error = "the compressed stream ends in a block header";
        return false;
    }
    const int literal_count = hlit + 257;
    const int dist_count    = hdist + 1;
    const int code_count    = hclen + 4;

    // The order the code-length code lengths arrive in. Spec-defined, not derivable.
    static constexpr int kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                       11, 4, 12, 3, 13, 2, 14, 1, 15};
    std::uint8_t code_lengths[19] = {};
    for (int i = 0; i < code_count; ++i) {
        const int value = bits.Bits(3);
        if (value < 0) {
            error = "the compressed stream ends in a code-length table";
            return false;
        }
        code_lengths[kOrder[i]] = static_cast<std::uint8_t>(value);
    }

    Huffman code_table;
    if (!code_table.Build(code_lengths, 19)) {
        error = "the code-length table in the compressed stream is not a valid Huffman code";
        return false;
    }

    std::vector<std::uint8_t> lengths(static_cast<std::size_t>(literal_count + dist_count), 0);
    for (int i = 0; i < literal_count + dist_count;) {
        const int symbol = bits.Decode(code_table);
        if (symbol < 0) {
            error = "the compressed stream ends in a code-length run";
            return false;
        }
        if (symbol < 16) {
            lengths[static_cast<std::size_t>(i++)] = static_cast<std::uint8_t>(symbol);
            continue;
        }

        int repeat = 0;
        std::uint8_t value = 0;
        if (symbol == 16) {
            if (i == 0) {
                error = "a repeat at the start of a code-length table has nothing to repeat";
                return false;
            }
            value = lengths[static_cast<std::size_t>(i - 1)];
            const int extra = bits.Bits(2);
            if (extra < 0) { error = "truncated code-length repeat"; return false; }
            repeat = 3 + extra;
        } else if (symbol == 17) {
            const int extra = bits.Bits(3);
            if (extra < 0) { error = "truncated code-length repeat"; return false; }
            repeat = 3 + extra;
        } else {
            const int extra = bits.Bits(7);
            if (extra < 0) { error = "truncated code-length repeat"; return false; }
            repeat = 11 + extra;
        }
        if (i + repeat > literal_count + dist_count) {
            error = "a code-length run overruns the table it is filling";
            return false;
        }
        while (repeat-- > 0) lengths[static_cast<std::size_t>(i++)] = value;
    }

    Huffman literals, distances;
    if (!literals.Build(lengths.data(), literal_count) ||
        !distances.Build(lengths.data() + literal_count, dist_count)) {
        error = "the compressed stream's Huffman tables are not valid codes";
        return false;
    }
    return InflateBlockBody(bits, literals, distances, out, error);
}

bool Inflate(std::string_view compressed, std::string& out, std::string& error) {
    BitReader bits(compressed);

    for (;;) {
        const int final_block = bits.Bits(1);
        const int type        = bits.Bits(2);
        if (final_block < 0 || type < 0) {
            error = "the compressed stream ends where a block should start";
            return false;
        }

        if (type == 0) {
            // Stored. Length and its complement, byte aligned.
            bits.AlignToByte();
            const std::size_t at = bits.at();
            if (at + 4 > compressed.size()) {
                error = "a stored block's header runs past the end of the file";
                return false;
            }
            const auto length = static_cast<std::size_t>(
                static_cast<std::uint8_t>(compressed[at]) |
                (static_cast<std::uint8_t>(compressed[at + 1]) << 8));
            if (at + 4 + length > compressed.size()) {
                error = "a stored block runs past the end of the file";
                return false;
            }
            out.append(compressed.substr(at + 4, length));
            bits.Seek(at + 4 + length);
        } else if (type == 1) {
            Huffman literals, distances;
            BuildFixedTables(literals, distances);
            if (!InflateBlockBody(bits, literals, distances, out, error)) return false;
        } else if (type == 2) {
            if (!InflateDynamic(bits, out, error)) return false;
        } else {
            error = "the compressed stream uses a reserved block type";
            return false;
        }

        if (final_block) return true;
        if (bits.Exhausted() && compressed.empty()) {
            error = "the compressed stream ends without a final block";
            return false;
        }
    }
}

} // namespace

bool LooksGzipped(std::string_view data) {
    return data.size() >= 2 && static_cast<std::uint8_t>(data[0]) == 0x1F &&
           static_cast<std::uint8_t>(data[1]) == 0x8B;
}

std::string GzipDecompress(std::string_view input, std::string& error) {
    error.clear();
    if (!LooksGzipped(input)) {
        error = "this is not a gzip file: it does not start with 1f 8b";
        return {};
    }
    if (input.size() < 18) {                 // header + a final empty block + trailer
        error = "this gzip file is too short to contain anything";
        return {};
    }
    if (static_cast<std::uint8_t>(input[2]) != 8) {
        error = "this gzip file uses a compression method other than deflate";
        return {};
    }

    const auto flags = static_cast<std::uint8_t>(input[3]);
    std::size_t at = 10;

    // Optional header fields, in the order RFC 1952 puts them.
    if (flags & 0x04) {                                  // FEXTRA
        if (at + 2 > input.size()) { error = "truncated gzip extra field"; return {}; }
        const auto extra = static_cast<std::size_t>(
            static_cast<std::uint8_t>(input[at]) |
            (static_cast<std::uint8_t>(input[at + 1]) << 8));
        at += 2 + extra;
    }
    for (const std::uint8_t flag : {std::uint8_t{0x08}, std::uint8_t{0x10}}) {  // FNAME, FCOMMENT
        if (!(flags & flag)) continue;
        while (at < input.size() && input[at] != '\0') ++at;
        ++at;
    }
    if (flags & 0x02) at += 2;                           // FHCRC
    if (at + 8 > input.size()) {
        error = "this gzip file has no room for a deflate stream";
        return {};
    }

    const std::size_t trailer = input.size() - 8;
    std::string out;

    // The trailer says how big it will be. Reserving that up front turns a 600 MB inflate
    // from a series of reallocations into one allocation.
    const auto expected_size = static_cast<std::uint32_t>(
        static_cast<std::uint8_t>(input[trailer + 4]) |
        (static_cast<std::uint8_t>(input[trailer + 5]) << 8) |
        (static_cast<std::uint8_t>(input[trailer + 6]) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(input[trailer + 7])) << 24));
    if (expected_size) out.reserve(expected_size);

    if (!Inflate(input.substr(at, trailer - at), out, error)) return {};

    // Both halves of the trailer, because a file that inflates to something it says is the
    // wrong size or the wrong checksum is not a file to hand to a parser.
    const auto expected_crc = static_cast<std::uint32_t>(
        static_cast<std::uint8_t>(input[trailer]) |
        (static_cast<std::uint8_t>(input[trailer + 1]) << 8) |
        (static_cast<std::uint8_t>(input[trailer + 2]) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(input[trailer + 3])) << 24));

    if (out.size() != expected_size) {
        error = "this gzip file says it holds " + std::to_string(expected_size) +
                " bytes but inflated to " + std::to_string(out.size());
        return {};
    }
    if (Crc32(out) != expected_crc) {
        error = "this gzip file fails its own checksum, so it is corrupt or truncated";
        return {};
    }
    return out;
}

bool GunzipFile(std::string_view in_path, std::string_view out_path, std::string& error) {
    std::ifstream in{std::string(in_path), std::ios::binary};
    if (!in) {
        error = "cannot open " + std::string(in_path);
        return false;
    }
    std::string packed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    const std::string plain = GzipDecompress(packed, error);
    if (!error.empty()) return false;

    std::ofstream out{std::string(out_path), std::ios::binary | std::ios::trunc};
    if (!out) {
        error = "cannot write " + std::string(out_path);
        return false;
    }
    out.write(plain.data(), static_cast<std::streamsize>(plain.size()));
    return static_cast<bool>(out);
}

} // namespace zircon::zdex
