// Tests for the Zdex client's local half: gzip, and later the JSON reader and config.
//
// The gzip tests do not check bytes against a golden blob. They write the output where an
// independent decompressor can read it back, because "my compressor agrees with my
// decompressor" proves nothing at all - and there is no decompressor here anyway. The
// harness script next to this file feeds every artefact to Python's zlib and compares.

#include "zdex/Config.h"
#include "zdex/Gzip.h"
#include "zdex/Sha256.h"
#include "zdex/Json.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace zircon;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expression);
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

std::filesystem::path OutDir() {
    return std::filesystem::temp_directory_path() / "zircon_zdex_test";
}

void Write(const std::filesystem::path& path, std::string_view data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Every case gets written out as <name>.raw and <name>.gz for the Python side to pair up.
void Case(const std::string& name, const std::string& data) {
    zdex::GzipStats stats;
    const std::string gz = zdex::GzipCompress(data, &stats);

    CHECK(gz.size() >= 18);                       // header + trailer, even when empty
    CHECK(static_cast<unsigned char>(gz[0]) == 0x1f);
    CHECK(static_cast<unsigned char>(gz[1]) == 0x8b);
    CHECK(static_cast<unsigned char>(gz[2]) == 0x08);
    CHECK(stats.raw == data.size());
    CHECK(stats.compressed == gz.size());

    Write(OutDir() / (name + ".raw"), data);
    Write(OutDir() / (name + ".gz"), gz);
}

std::string Repeat(std::string_view unit, std::size_t times) {
    std::string out;
    out.reserve(unit.size() * times);
    for (std::size_t i = 0; i < times; ++i) out += unit;
    return out;
}

// ---------------------------------------------------------------------------------
// Reading it back
// ---------------------------------------------------------------------------------

// Round trip through our own compressor. 0.7.0 shipped the writer without the reader and
// four commands could not open what the batch runner wrote, so this is the test that would
// have caught it.
void RoundTrip(std::string_view label, const std::string& data) {
    const std::string packed = zdex::GzipCompress(data);
    CHECK(zdex::LooksGzipped(packed));

    std::string error;
    const std::string back = zdex::GzipDecompress(packed, error);
    if (!error.empty()) {
        std::fprintf(stderr, "      %.*s: %s\n", static_cast<int>(label.size()), label.data(),
                     error.c_str());
    }
    CHECK(error.empty());
    CHECK(back.size() == data.size());
    CHECK(back == data);
}

void TestRoundTrip() {
    RoundTrip("empty", "");
    RoundTrip("one", "x");
    RoundTrip("three", "abc");
    RoundTrip("nomatch", "abcdefghijklmnopqrstuvwxyz0123456789");
    RoundTrip("allsame", std::string(70000, 'A'));      // overlapping back-references
    RoundTrip("repeated", Repeat("the quick brown fox ", 5000));

    // Something shaped like what this actually carries.
    std::string json = "{\"schema_version\": 3, \"packages\": [";
    for (int i = 0; i < 2000; ++i)
        json += "{\"name\": \"Assembly-CSharp\", \"path\": \"Game.Type" + std::to_string(i) +
                ", Assembly-CSharp\"},";
    json += "]}";
    RoundTrip("dumpish", json);

    // Every byte value, so nothing depends on the input being text.
    std::string bytes;
    for (int i = 0; i < 256; ++i) bytes.push_back(static_cast<char>(i));
    RoundTrip("allbytes", Repeat(bytes, 40));
}

// A gzip file this compressor could never have produced. GzipCompress only emits fixed-Huffman
// blocks; anything gzipped by Python, gzip(1) or 7-Zip uses dynamic ones, and a reader that
// only handled what we write would fail on every file a user brings.
void TestForeignGzip() {
    const auto tools = OutDir() / "foreign";
    std::error_code ec;
    std::filesystem::create_directories(tools, ec);

    // Built by hand rather than shelled out to, so the suite still needs nothing installed:
    // a stored block, which is legal DEFLATE and which our own writer never emits either.
    const std::string payload = "stored blocks are legal and we never write one";

    std::string file;
    file += '\x1f'; file += '\x8b'; file += '\x08'; file += '\x00';   // magic, deflate, no flags
    file += std::string(6, '\x00');                                    // mtime, xfl, os

    file += '\x01';                                                    // final, stored
    const auto n = static_cast<std::uint16_t>(payload.size());
    file += static_cast<char>(n & 0xFF);
    file += static_cast<char>((n >> 8) & 0xFF);
    file += static_cast<char>(~n & 0xFF);
    file += static_cast<char>((~n >> 8) & 0xFF);
    file += payload;

    const std::uint32_t crc = zdex::Crc32(payload);
    for (int i = 0; i < 4; ++i) file += static_cast<char>((crc >> (i * 8)) & 0xFF);
    for (int i = 0; i < 4; ++i) file += static_cast<char>((n >> (i * 8)) & 0xFF);

    std::string error;
    const std::string back = zdex::GzipDecompress(file, error);
    CHECK(error.empty());
    CHECK(back == payload);
}

// Refusals. A file that inflates to something it says is wrong is not a file to parse.
void TestGunzipRefusals() {
    std::string error;

    CHECK(zdex::GzipDecompress("not gzip at all", error).empty());
    CHECK(!error.empty());
    CHECK(!zdex::LooksGzipped("not gzip at all"));

    // Truncated.
    const std::string packed = zdex::GzipCompress(Repeat("abcdefgh", 4000));
    CHECK(zdex::GzipDecompress(packed.substr(0, packed.size() / 2), error).empty());
    CHECK(!error.empty());

    // Corrupt payload, intact trailer: the CRC has to catch it.
    std::string bent = packed;
    bent[bent.size() / 2] = static_cast<char>(bent[bent.size() / 2] ^ 0xFF);
    const std::string out = zdex::GzipDecompress(bent, error);
    CHECK(!error.empty() || out.empty());

    // A good file with a wrong length in its trailer.
    std::string lied = packed;
    lied[lied.size() - 1] = static_cast<char>(lied[lied.size() - 1] ^ 0x5A);
    CHECK(zdex::GzipDecompress(lied, error).empty());
    CHECK(!error.empty());
}

void TestGunzipFile() {
    const auto raw = OutDir() / "gunzip-in.txt";
    const auto gz  = OutDir() / "gunzip-in.txt.gz";
    const auto out = OutDir() / "gunzip-out.txt";

    const std::string data = Repeat("round and round it goes ", 3000);
    Write(raw, data);

    std::string error;
    CHECK(zdex::GzipFile(raw.string(), gz.string(), error));
    CHECK(error.empty());
    CHECK(zdex::GunzipFile(gz.string(), out.string(), error));
    CHECK(error.empty());

    std::ifstream back(out, std::ios::binary);
    const std::string got((std::istreambuf_iterator<char>(back)),
                          std::istreambuf_iterator<char>());
    CHECK(got == data);
}

// ---------------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------------

// The published vectors, plus the lengths around the block boundary where padding goes
// wrong: 55 bytes still fits the length field, 56 forces a second block, 64 is exact.
void TestSha256() {
    CHECK(zdex::Sha256Hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(zdex::Sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(zdex::Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    CHECK(zdex::Sha256Hex(std::string(55, 'a')) ==
          "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(zdex::Sha256Hex(std::string(56, 'a')) ==
          "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(zdex::Sha256Hex(std::string(64, 'a')) ==
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");

    CHECK(zdex::Sha256Hex(Repeat("a", 1000000)) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    // Every byte value, so nothing depends on the input being text.
    std::string bytes;
    for (int i = 0; i < 256; ++i) bytes.push_back(static_cast<char>(i));
    CHECK(zdex::Sha256Hex(bytes) ==
          "40aff2e9d2d8922e47afd4648e6967497158785fbd1da870e7110266bf944880");

    // A file and the same bytes in memory have to agree, or a skipped upload is skipped
    // against the wrong number.
    const auto path = OutDir() / "sha-input.bin";
    const std::string data = Repeat("zircon ", 30000);
    Write(path, data);
    CHECK(zdex::Sha256File(path.string()) == zdex::Sha256Hex(data));

    CHECK(zdex::Sha256File((OutDir() / "no-such-file").string()).empty());
}

void TestCrc32KnownVectors() {
    // The values every CRC32 implementation is checked against.
    CHECK(zdex::Crc32("") == 0x00000000u);
    CHECK(zdex::Crc32("a") == 0xE8B7BE43u);
    CHECK(zdex::Crc32("abc") == 0x352441C2u);
    CHECK(zdex::Crc32("123456789") == 0xCBF43926u);
    CHECK(zdex::Crc32("The quick brown fox jumps over the lazy dog") == 0x414FA339u);

    // Seeding has to chain, or the streaming path computes a different digest from the
    // in-memory one for the same bytes.
    const std::string whole = "The quick brown fox jumps over the lazy dog";
    const std::uint32_t part = zdex::Crc32(whole.substr(0, 10));
    CHECK(zdex::Crc32(whole.substr(10), part) == zdex::Crc32(whole));
}

void TestShapes() {
    Case("empty", "");
    Case("one", "x");
    Case("two", "ab");
    Case("three", "abc");                          // exactly the minimum match length
    Case("nomatch", "abcdefghijklmnopqrstuvwxyz0123456789");
    Case("allsame", std::string(70000, 'A'));      // longer than one max-length match
    Case("maxmatch", std::string(258, 'Q') + "tail");
    Case("nearmax", std::string(257, 'Q') + "tail");
    Case("overmax", std::string(259, 'Q') + "tail");

    // Every byte value, so the 9-bit literal codes (144-255) are exercised.
    std::string all;
    for (int i = 0; i < 256; ++i) all.push_back(static_cast<char>(i));
    Case("allbytes", Repeat(all, 8));

    // A distance just past the window, which must not be emitted as a match.
    Case("window", "MARKER" + std::string(40000, '.') + "MARKER");

    // JSON, which is what this actually compresses in anger.
    Case("json", Repeat(R"({"name":"Actor","offset":120,"size":8,"type":"objectptr"},)", 4000));

    // Incompressible, where deflate must not make things much worse.
    std::mt19937 rng(1234);
    std::string noise(200000, '\0');
    for (char& c : noise) c = static_cast<char>(rng() & 0xFF);
    Case("random", noise);

    // Crosses the 4 MB slab boundary, so more than one block is emitted and the bit
    // stream has to run on unaligned between them.
    Case("multiblock", Repeat("The quick brown fox jumps over the lazy dog. ", 260000));
}

void TestFileStreaming() {
    // The streaming path and the in-memory path must agree byte for byte, or a published
    // dump differs from what a test compressed.
    const std::string data = Repeat(R"({"path":"/Script/Engine.Actor","members":42},)", 300000);

    const auto raw_path = OutDir() / "streamed.raw";
    const auto gz_path = OutDir() / "streamed.gz";
    Write(raw_path, data);

    std::string error;
    zdex::GzipStats stats;
    std::uint64_t last_progress = 0;
    const bool ok = zdex::GzipFile(raw_path.string(), gz_path.string(), error, &stats,
                                   [&](std::uint64_t seen) {
                                       CHECK(seen >= last_progress);
                                       last_progress = seen;
                                       return true;
                                   });
    CHECK(ok);
    CHECK(error.empty());
    CHECK(stats.raw == data.size());
    CHECK(last_progress == data.size());

    const std::string streamed = Read(gz_path);
    const std::string in_memory = zdex::GzipCompress(data);
    CHECK(streamed == in_memory);
    CHECK(stats.compressed == streamed.size());
}

void TestAbortRemovesOutput() {
    const std::string data = Repeat("abcdefgh", 800000);
    const auto raw_path = OutDir() / "aborted.raw";
    const auto gz_path = OutDir() / "aborted.gz";
    Write(raw_path, data);

    std::string error;
    const bool ok = zdex::GzipFile(raw_path.string(), gz_path.string(), error, nullptr,
                                   [](std::uint64_t) { return false; });
    CHECK(!ok);
    CHECK(error == "cancelled");
    // A half-written .gz left behind would later be uploaded as if it were whole.
    CHECK(!std::filesystem::exists(gz_path));
}

void TestMissingInput() {
    std::string error;
    const bool ok = zdex::GzipFile("no_such_file_anywhere.json",
                                   (OutDir() / "never.gz").string(), error);
    CHECK(!ok);
    CHECK(!error.empty());
}

void TestRatioIsWorthHaving() {
    // The whole point of writing a compressor rather than uploading plain JSON.
    const std::string json = Repeat(
        R"({"name":"MaxWalkSpeed","offset":632,"size":4,"type":{"kind":"float","raw":"FloatProperty","size":4}},)",
        20000);
    zdex::GzipStats stats;
    zdex::GzipCompress(json, &stats);
    std::fprintf(stderr, "  json ratio: %.1fx (%llu -> %llu)\n", stats.ratio(),
                 static_cast<unsigned long long>(stats.raw),
                 static_cast<unsigned long long>(stats.compressed));
    CHECK(stats.ratio() > 4.0);
}


// ── JSON ───────────────────────────────────────────────────────────────────────────

zdex::JsonValue Parsed(std::string_view text) {
    zdex::JsonValue v;
    std::string error;
    CHECK(zdex::ParseJson(text, v, error));
    return v;
}

void TestJsonRealResponses() {
    // The exact shapes from the handoff's live test log.
    const auto init = Parsed(R"({"ok":true,"upload_id":"0750035d86409bdec8454f717b808b60",)"
                             R"("chunk_bytes":8388608,"chunk_count":4,"received":[]})");
    CHECK(init.Bool("ok"));
    CHECK(init.Str("upload_id") == "0750035d86409bdec8454f717b808b60");
    CHECK(init.Int("chunk_bytes") == 8388608);
    CHECK(init.Int("chunk_count") == 4);
    CHECK(init.IntArray("received").empty());

    const auto partial = Parsed(R"({"ok":false,"error":"incomplete","received":[0,1,3]})");
    CHECK(!partial.Bool("ok", true));
    CHECK(partial.Str("error") == "incomplete");
    const auto got = partial.IntArray("received");
    CHECK(got.size() == 3);
    CHECK(got[0] == 0 && got[1] == 1 && got[2] == 3);

    const auto finish = Parsed(R"({"ok":true,"dump_id":42,)"
                               R"("url":"https://zlogic.eu/zdex/d/42","status":"queued"})");
    CHECK(finish.Int("dump_id") == 42);
    CHECK(finish.Str("url") == "https://zlogic.eu/zdex/d/42");

    const auto status = Parsed(R"({"ok":true,"id":42,"status":"importing","progress":63,)"
                               R"("phase":"members","phase_label":"Reading members","error":null})");
    CHECK(status.Int("progress") == 63);
    CHECK(status.Str("phase_label") == "Reading members");
    CHECK(status["error"].IsNull());
    CHECK(status.Str("error").empty());

    const auto err = Parsed(R"({"ok":false,"error":"invalid_key","message":"That key is not valid."})");
    CHECK(err.Str("message") == "That key is not valid.");
}

void TestJsonMissingFieldsAreSafe() {
    // Every accessor must give a default rather than blowing up, so the client can read a
    // response the server trimmed without checking each field first.
    const auto empty = Parsed("{}");
    CHECK(empty.Str("nope").empty());
    CHECK(empty.Str("nope", "fallback") == "fallback");
    CHECK(empty.Int("nope", -7) == -7);
    CHECK(empty.Bool("nope", true));
    CHECK(empty.IntArray("nope").empty());
    CHECK(empty["a"]["b"]["c"].IsNull());    // chains through missing keys
}

void TestJsonRejectsNonJson() {
    // This is the Cloudflare case: a 403 whose body is plain text. It must be reported as
    // "not JSON", not silently read as an empty object.
    zdex::JsonValue v;
    std::string error;
    CHECK(!zdex::ParseJson("error code: 1010", v, error));
    CHECK(!error.empty());
    CHECK(!zdex::ParseJson("", v, error));
    CHECK(!zdex::ParseJson("<html><body>Blocked</body></html>", v, error));
    CHECK(!zdex::ParseJson("{\"ok\":", v, error));
    CHECK(!zdex::ParseJson("{\"a\" 1}", v, error));
}

void TestJsonStringsAndEscapes() {
    // Raw string literals, so what the parser sees is exactly what a server sends rather
    // than what the C++ compiler already unescaped on its behalf.
    const auto v = Parsed(
        R"({"s":"quote \" back \\ slash \/ nl \n tab \t",)"
        R"("u":"\u00e9\u2014","emoji":"\ud83d\ude00",)"
        R"("n":-12.5,"big":9007199254740992})");

    CHECK(v.Str("s") == "quote \" back \\ slash / nl \n tab \t");
    CHECK(v.Str("u") == "\xC3\xA9\xE2\x80\x94");      // e-acute, em dash
    CHECK(v.Str("emoji") == "\xF0\x9F\x98\x80");  // surrogate pair -> one code point
    CHECK(v.Int("n") == -12);
    CHECK(v.Int("big") == 9007199254740992LL);

    // Round-trip through the writer the request bodies use. A game title really can
    // contain a quote or a backslash and it goes straight into an init body.
    const std::string awkward = "He said \"hi\"\\ \n\t and a \x01 control";
    const std::string encoded = "{\"v\": \"" + zdex::JsonEscape(awkward) + "\"}";
    CHECK(Parsed(encoded).Str("v") == awkward);
}

void TestJsonNesting() {
    const auto v = Parsed(R"({"games":[{"id":1,"name":"A"},{"id":2,"name":"B"}]})");
    const auto& games = v["games"];
    CHECK(games.items.size() == 2);
    CHECK(games.items[1].Str("name") == "B");
    CHECK(games.items[1].Int("id") == 2);

    // A runaway nesting depth must be refused rather than recursing until the stack goes.
    std::string deep;
    for (int i = 0; i < 200; ++i) deep += "[";
    zdex::JsonValue out;
    std::string error;
    CHECK(!zdex::ParseJson(deep, out, error));
}

// ── config ─────────────────────────────────────────────────────────────────────────

void TestKeyHintNeverLeaksTheKey() {
    zdex::Config config;
    config.api_key = "zdx_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcd";
    const std::string hint = config.KeyHint();
    CHECK(hint.find("ABCDEFGHIJKLMNOPQRST") == std::string::npos);
    CHECK(hint.rfind("zdx_", 0) == 0);
    CHECK(hint.size() < config.api_key.size());

    zdex::Config none;
    CHECK(none.KeyHint() == "(none)");
    CHECK(!none.HasKey());
}

void TestApiBaseAndTrailingSlash() {
    zdex::Config config;
    CHECK(config.base_url == std::string(zdex::kDefaultBaseUrl));
    CHECK(config.ApiBase() == "https://zlogic.eu/zdex/api/v1");
}

void TestFingerprintChangesWithTheFile() {
    const auto a = OutDir() / "fp_a.json";
    Write(a, "{\"schema_version\":1}");
    const std::string first = zdex::FingerprintFile(a.string());
    CHECK(!first.empty());
    CHECK(first == zdex::FingerprintFile(a.string()));   // stable for the same file

    // A different size must fingerprint differently, or a re-dumped game would resume
    // into the previous file's chunks and upload a mixture of the two.
    Write(a, "{\"schema_version\":1,\"packages\":[]}");
    CHECK(zdex::FingerprintFile(a.string()) != first);

    CHECK(zdex::FingerprintFile("no_such_file_at_all.json").empty());
}

} // namespace

int main() {
    std::error_code ec;
    std::filesystem::create_directories(OutDir(), ec);

    TestJsonRealResponses();
    TestJsonMissingFieldsAreSafe();
    TestJsonRejectsNonJson();
    TestJsonStringsAndEscapes();
    TestJsonNesting();
    TestKeyHintNeverLeaksTheKey();
    TestApiBaseAndTrailingSlash();
    TestFingerprintChangesWithTheFile();

    TestCrc32KnownVectors();
    TestShapes();
    TestFileStreaming();
    TestAbortRemovesOutput();
    TestMissingInput();
    TestRatioIsWorthHaving();
    TestRoundTrip();
    TestForeignGzip();
    TestGunzipRefusals();
    TestGunzipFile();
    TestSha256();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    std::printf("artefacts in %s\n", OutDir().string().c_str());
    return g_failures == 0 ? 0 : 1;
}
