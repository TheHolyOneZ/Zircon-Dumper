#include "zdex/Sha256.h"

#include <array>
#include <cstring>
#include <fstream>
#include <vector>

namespace zircon::zdex {
namespace {

// FIPS 180-4. The constants are the first 32 bits of the fractional parts of the cube roots
// of the first sixty-four primes; the initial state is the same thing for square roots.
constexpr std::uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

std::uint32_t Rotate(std::uint32_t value, int by) {
    return (value >> by) | (value << (32 - by));
}

struct Hasher {
    std::array<std::uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> buffer{};
    std::size_t   pending{0};
    std::uint64_t total{0};

    void Block(const std::uint8_t* at) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(at[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(at[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(at[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(at[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = Rotate(w[i - 15], 7) ^ Rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = Rotate(w[i - 2], 17) ^ Rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        auto [a, b, c, d, e, f, g, h] = state;
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = h + s1 + ch + kRound[i] + w[i];
            const std::uint32_t s0 = Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;

            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    void Add(const std::uint8_t* data, std::size_t size) {
        total += size;
        while (size > 0) {
            const std::size_t room = 64 - pending;
            const std::size_t take = size < room ? size : room;
            std::memcpy(buffer.data() + pending, data, take);
            pending += take;
            data    += take;
            size    -= take;
            if (pending == 64) {
                Block(buffer.data());
                pending = 0;
            }
        }
    }

    std::string Finish() {
        // The padding is a 1 bit, then zeros, then the length in bits as a big-endian 64.
        const std::uint64_t bits = total * 8;
        std::uint8_t one = 0x80;
        Add(&one, 1);
        total -= 1;                       // the padding is not message length

        std::uint8_t zero = 0;
        while (pending != 56) {
            Add(&zero, 1);
            total -= 1;
        }

        std::uint8_t tail[8];
        for (int i = 0; i < 8; ++i) tail[i] = static_cast<std::uint8_t>(bits >> (56 - i * 8));
        Add(tail, 8);

        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (const std::uint32_t word : state) {
            for (int shift = 28; shift >= 0; shift -= 4)
                out.push_back(kHex[(word >> shift) & 0xF]);
        }
        return out;
    }
};

} // namespace

std::string Sha256Hex(std::string_view data) {
    Hasher hasher;
    hasher.Add(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    return hasher.Finish();
}

std::string Sha256File(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};

    Hasher hasher;
    std::vector<char> chunk(1 << 16);
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) break;
        hasher.Add(reinterpret_cast<const std::uint8_t*>(chunk.data()), got);
    }
    return hasher.Finish();
}

} // namespace zircon::zdex
