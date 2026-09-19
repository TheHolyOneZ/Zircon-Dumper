#include "il2cpp/Metadata.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <map>
#include <set>

namespace zircon::il2cpp {
namespace {

using core::Error;

constexpr std::uint32_t kSanity   = 0xFAB11BAF;
constexpr std::size_t   kPrologue = 8;      // sanity + version

// Alignment between spans, not a hole. A reading that needs more than this per span is not
// the right reading.
constexpr std::uint32_t kMaxPaddingPerSpan = 64;

// Record sizes to try. IL2CPP records are int32-aligned and none of them is anywhere near
// this wide.
constexpr int kMinRecord = 8;
constexpr int kMaxRecord = 136;

// Consecutive records first. A stride that divides the real record size only survives
// probing when every probe lands on a real boundary, and consecutive probes cannot.
constexpr std::size_t kDenseProbes  = 600;
constexpr std::size_t kSpreadProbes = 400;

// Below this a span is too small to tell apart from noise.
constexpr std::uint32_t kMinTableBytes = 256;
constexpr std::uint32_t kMinRecords    = 16;

std::int32_t ReadI32(std::span<const std::uint8_t> file, std::size_t at) {
    std::int32_t value = 0;
    std::memcpy(&value, file.data() + at, sizeof(value));
    return value;
}

std::uint16_t ReadU16(std::span<const std::uint8_t> file, std::size_t at) {
    std::uint16_t value = 0;
    std::memcpy(&value, file.data() + at, sizeof(value));
    return value;
}

// Bytes that occur in managed identifiers. Generous on purpose -- generic names carry
// backticks, angle brackets and commas, and a namespace carries dots.
bool IdentifierByte(std::uint8_t b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') ||
           b == '_' || b == '.' || b == '<' || b == '>' || b == '`' || b == '|' ||
           b == '=' || b == '-' || b == '+' || b == '[' || b == ']' || b == '{' ||
           b == '}' || b == '(' || b == ')' || b == '$' || b == ',' || b == ':' ||
           b == '/' || b == ' ';
}

struct Reading {
    int ints_per_entry{0};
    std::vector<MetadataSpan> spans;
    std::uint32_t padding{0};
    std::uint32_t slack{0};
};

// Constraint 1: the spans tile the file.
std::optional<Reading> TryStride(std::span<const std::uint8_t> file, int ints_per_entry) {
    const auto n = static_cast<std::uint32_t>(file.size());
    const std::size_t stride = static_cast<std::size_t>(ints_per_entry) * 4;

    const std::int32_t header_length = ReadI32(file, kPrologue);
    if (header_length <= static_cast<std::int32_t>(kPrologue) ||
        static_cast<std::uint32_t>(header_length) > n)
        return std::nullopt;
    if ((static_cast<std::size_t>(header_length) - kPrologue) % stride != 0) return std::nullopt;

    const std::size_t count = (static_cast<std::size_t>(header_length) - kPrologue) / stride;
    if (count == 0) return std::nullopt;

    Reading reading;
    reading.ints_per_entry = ints_per_entry;
    reading.spans.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t at = kPrologue + i * stride;
        const std::int32_t offset = ReadI32(file, at);
        const std::int32_t size   = ReadI32(file, at + 4);
        if (offset < 0 || size < 0) return std::nullopt;
        if (static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(size) > n)
            return std::nullopt;
        reading.spans.push_back(MetadataSpan{static_cast<std::uint32_t>(offset),
                                             static_cast<std::uint32_t>(size)});
    }

    auto ordered = reading.spans;
    std::sort(ordered.begin(), ordered.end(),
              [](const MetadataSpan& a, const MetadataSpan& b) { return a.offset < b.offset; });

    std::uint32_t cursor = static_cast<std::uint32_t>(header_length);
    for (const auto& span : ordered) {
        if (span.offset < cursor) return std::nullopt;          // overlap
        reading.padding += span.offset - cursor;
        cursor = std::max(cursor, span.offset + span.size);
    }
    if (reading.padding > kMaxPaddingPerSpan * count) return std::nullopt;

    reading.slack = n - cursor;
    return reading;
}

// Constraint 2: the identifier blob.
int FindStringBlob(std::span<const std::uint8_t> file, const std::vector<MetadataSpan>& spans,
                   double& density_out) {
    int best_index = -1;
    double best = 0.0;

    for (std::size_t i = 0; i < spans.size(); ++i) {
        const auto& span = spans[i];
        if (span.size < 1024) continue;
        if (file[span.offset + span.size - 1] != 0) continue;   // a blob ends terminated

        std::size_t identifiers = 0;
        std::size_t terminators = 0;
        for (std::uint32_t at = 0; at < span.size; ++at) {
            const std::uint8_t b = file[span.offset + at];
            if (b == 0) ++terminators;
            else if (IdentifierByte(b)) ++identifiers;
        }
        const double density   = static_cast<double>(identifiers) / span.size;
        const double nul_rate  = static_cast<double>(terminators) / span.size;
        if (density < 0.80 || nul_rate <= 0.02 || nul_rate >= 0.35) continue;
        if (density > best) {
            best = density;
            best_index = static_cast<int>(i);
        }
    }
    density_out = best;
    return best_index;
}

// Every offset in the blob that begins a string.
std::set<std::uint32_t> StringStarts(std::span<const std::uint8_t> file, const MetadataSpan& blob) {
    std::set<std::uint32_t> starts{0};
    for (std::uint32_t at = 0; at + 1 < blob.size; ++at)
        if (file[blob.offset + at] == 0) starts.insert(at + 1);
    return starts;
}

std::vector<std::uint32_t> ProbeRows(std::uint32_t count) {
    std::vector<std::uint32_t> rows;
    const std::uint32_t dense = std::min<std::uint32_t>(count, kDenseProbes);
    for (std::uint32_t r = 0; r < dense; ++r) rows.push_back(r);
    if (count > dense) {
        std::uint32_t step = std::max<std::uint32_t>(1, count / kSpreadProbes);
        if (step % 2 == 0) ++step;          // odd, so it cannot share every factor with a record
        for (std::uint32_t r = dense; r < count; r += step) rows.push_back(r);
    }
    return rows;
}

// Constraint 3: a slot near the front of the record holds a name index, for every record.
//
// Near the front rather than first. The name is slot 0 in every build in the corpus, but
// nothing says it has to be, and scanning the first few slots costs four reads instead of
// one where scanning all of them would cost thirty.
constexpr int kNameSlotSearch = 4;

std::vector<int> NameSlotsIn(std::span<const std::uint8_t> file, const MetadataSpan& span,
                             int record, const MetadataSpan& blob,
                             const std::set<std::uint32_t>& starts) {
    std::vector<int> found;
    if (record <= 0) return found;

    // No exact-multiple requirement. Not every build makes a span a whole number of records:
    // one in the corpus leaves 18 bytes on the end, and insisting on an exact divide threw
    // that game's type table away. Whatever is left over is padding, and the probing below
    // is what actually rejects a wrong record size.
    const std::uint32_t count = span.size / static_cast<std::uint32_t>(record);
    if (count < kMinRecords) return found;

    const auto rows = ProbeRows(count);
    const int slots = std::min(kNameSlotSearch, record / 4);
    for (int slot = 0; slot < slots; ++slot) {
        bool all = true;

        // A column that never changes is not a name column. Zero is a valid string index, so
        // a run of zeroes passes every other test here and will happily be mistaken for a
        // table of names that are all the first name.
        std::vector<std::int32_t> seen;
        for (const std::uint32_t row : rows) {
            const std::int32_t value = ReadI32(
                file, span.offset + static_cast<std::size_t>(row) * record +
                          static_cast<std::size_t>(slot) * 4);
            if (value < 0 || static_cast<std::uint32_t>(value) >= blob.size ||
                !starts.count(static_cast<std::uint32_t>(value))) {
                all = false;
                break;
            }
            if (seen.size() < 4 && std::find(seen.begin(), seen.end(), value) == seen.end())
                seen.push_back(value);
        }
        if (all && seen.size() > 1) found.push_back(slot);
        if (found.size() == 2) break;           // a name and a namespace is all anything needs
    }
    return found;
}

struct NameTable {
    int record{0};
    std::uint32_t count{0};
    std::vector<int> name_slots;        // where the names are, in record order

    int leading() const { return static_cast<int>(name_slots.size()); }
};

// Every record size that reads as a name table, not just the first.
//
// One size per span was enough until a build turned up whose type span divides evenly by no
// plausible record at all -- it carries 14 bytes of padding -- so which size is right has to
// be settled by the partition constraint further down rather than here. Exact divisors come
// first, because when one exists it is almost always the answer.
std::map<int, std::vector<NameTable>> FindNameTables(std::span<const std::uint8_t> file,
                                                    const std::vector<MetadataSpan>& spans,
                                                    int blob_index, const MetadataSpan& blob,
                                                    const std::set<std::uint32_t>& starts) {
    std::map<int, std::vector<NameTable>> found;
    for (std::size_t i = 0; i < spans.size(); ++i) {
        if (static_cast<int>(i) == blob_index) continue;
        const auto& span = spans[i];
        if (span.size < kMinTableBytes) continue;

        std::vector<NameTable> exact, padded;
        for (int record = kMinRecord; record <= kMaxRecord; record += 4) {
            auto slots = NameSlotsIn(file, span, record, blob, starts);
            if (slots.empty()) continue;

            const NameTable table{record, span.size / static_cast<std::uint32_t>(record),
                                  std::move(slots)};
            (span.size % static_cast<std::uint32_t>(record) == 0 ? exact : padded)
                .push_back(table);
        }
        auto& into = found[static_cast<int>(i)];
        into = std::move(exact);
        into.insert(into.end(), padded.begin(), padded.end());
        if (into.empty()) found.erase(static_cast<int>(i));
    }
    return found;
}

// The one a caller wants when it only wants one: the first, which is the smallest exact
// divisor where there is one.
const NameTable* Primary(const std::map<int, std::vector<NameTable>>& tables, int span) {
    const auto it = tables.find(span);
    return it == tables.end() || it->second.empty() ? nullptr : &it->second.front();
}

// Constraint 4: (start, count) partitions [0, N) exactly once.
//
// Sorted, not in record order. Fields and methods are not laid out in type order -- an
// earlier version of this required they were and found nothing at all.
std::optional<std::uint32_t> Partitions(const std::vector<std::int32_t>& start,
                                        const std::vector<std::uint16_t>& count) {
    std::vector<std::pair<std::int32_t, std::uint16_t>> ranges;
    ranges.reserve(start.size());
    for (std::size_t i = 0; i < start.size(); ++i) {
        if (start[i] == -1) {
            if (count[i] != 0) return std::nullopt;     // no start, but a count: not this pair
            continue;
        }
        if (start[i] < 0) return std::nullopt;
        if (count[i] == 0) continue;
        ranges.emplace_back(start[i], count[i]);
    }
    if (ranges.size() < 8) return std::nullopt;

    std::sort(ranges.begin(), ranges.end());
    std::uint32_t cursor = 0;
    for (const auto& [at, many] : ranges) {
        if (static_cast<std::uint32_t>(at) != cursor) return std::nullopt;
        cursor += many;
    }
    return cursor;
}

// What record sizes a span could plausibly be made of, given a record count.
bool SpanHolds(const MetadataSpan& span, std::uint32_t records, int& record_out) {
    if (records == 0 || span.size == 0) return false;
    const std::uint32_t record = span.size / records;
    if (record < 2 || record > static_cast<std::uint32_t>(kMaxRecord)) return false;
    // Same tolerance as above: a remainder smaller than one record is trailing padding.
    if (span.size - record * records >= record) return false;
    record_out = static_cast<int>(record);
    return true;
}

std::vector<MetadataRange> FindRanges(std::span<const std::uint8_t> file,
                                      const std::vector<MetadataSpan>& spans, int table_index,
                                      int record) {
    const auto& span = spans[static_cast<std::size_t>(table_index)];
    const std::uint32_t count = span.size / static_cast<std::uint32_t>(record);
    const int int_slots   = record / 4;
    const int short_slots = record / 2;

    // Columns once, because every pair of them gets compared.
    std::vector<std::vector<std::int32_t>> ints(static_cast<std::size_t>(int_slots));
    std::vector<std::vector<std::uint16_t>> shorts(static_cast<std::size_t>(short_slots));
    for (auto& column : ints)   column.resize(count);
    for (auto& column : shorts) column.resize(count);

    for (std::uint32_t r = 0; r < count; ++r) {
        const std::size_t base = span.offset + static_cast<std::size_t>(r) * record;
        for (int c = 0; c < int_slots; ++c)
            ints[static_cast<std::size_t>(c)][r] = ReadI32(file, base + static_cast<std::size_t>(c) * 4);
        for (int c = 0; c < short_slots; ++c)
            shorts[static_cast<std::size_t>(c)][r] = ReadU16(file, base + static_cast<std::size_t>(c) * 2);
    }

    std::vector<MetadataRange> found;
    for (int a = 0; a < int_slots; ++a) {
        const auto& start = ints[static_cast<std::size_t>(a)];
        // A start column holds indices: -1 or a non-negative number, and it has to reach
        // somewhere. A column of flags or tokens does not survive the partition below, but
        // skipping the obviously-wrong ones first keeps this cheap.
        if (std::any_of(start.begin(), start.end(), [](std::int32_t v) { return v < -1; }))
            continue;

        for (int b = 0; b < short_slots; ++b) {
            const auto total = Partitions(start, shorts[static_cast<std::size_t>(b)]);
            if (!total) continue;

            for (std::size_t t = 0; t < spans.size(); ++t) {
                if (static_cast<int>(t) == table_index) continue;
                int record_size = 0;
                if (!SpanHolds(spans[t], *total, record_size)) continue;
                found.push_back(MetadataRange{a, b, static_cast<int>(t), *total});
                break;                      // one span per range; ambiguity is caught below
            }
        }
    }
    return found;
}

} // namespace

bool LooksLikeMetadata(std::span<const std::uint8_t> file) {
    if (file.size() < kPrologue + 8) return false;
    std::uint32_t sanity = 0;
    std::memcpy(&sanity, file.data(), sizeof(sanity));
    return sanity == kSanity;
}

core::Result<MetadataLayout> SolveMetadataLayout(std::span<const std::uint8_t> file) {
    if (file.size() < 1024)
        return Error{"this file is too small to be global-metadata.dat", 6};
    if (!LooksLikeMetadata(file))
        return Error{"the first four bytes are not 0xFAB11BAF, so this is either not "
                     "global-metadata.dat or it has been encrypted or packed", 6};

    MetadataLayout layout;
    layout.version = ReadI32(file, 4);

    // --- constraint 1: the spans tile the file --------------------------------------
    std::optional<Reading> best;
    for (const int ints : {2, 3, 4}) {
        auto reading = TryStride(file, ints);
        if (!reading) continue;
        if (!best || std::pair(reading->slack, reading->padding) <
                     std::pair(best->slack, best->padding))
            best = std::move(reading);
    }
    if (!best)
        return Error{std::format("no reading of the header tiles the file, so the span table "
                                 "is not where it should be (metadata version {})",
                                 layout.version), 6};

    layout.ints_per_entry = best->ints_per_entry;
    layout.spans          = std::move(best->spans);
    layout.evidence.push_back(std::format(
        "header tiles the file with {} int32 per entry and {} spans, {} bytes of padding and "
        "{} left over", layout.ints_per_entry, layout.spans.size(), best->padding, best->slack));

    // --- constraint 2: the identifier blob -------------------------------------------
    double density = 0.0;
    layout.tables.strings = FindStringBlob(file, layout.spans, density);
    if (layout.tables.strings < 0)
        return Error{"no span is a blob of NUL-terminated identifiers, so the string table "
                     "could not be found", 6};

    const auto& blob = layout.Span(layout.tables.strings);
    const auto starts = StringStarts(file, blob);
    layout.evidence.push_back(std::format(
        "span {} is the identifier blob: {} names, {:.0f}% identifier bytes",
        layout.tables.strings, starts.size(), density * 100.0));

    // --- constraint 3: which spans are record tables ---------------------------------
    const auto name_tables = FindNameTables(file, layout.spans, layout.tables.strings, blob, starts);
    if (name_tables.empty())
        return Error{"no span reads as a table of records beginning with a name index", 6};

    // --- constraint 4: the type table is the one whose ranges partition the most ------
    //
    // Every candidate record size gets tried, not just the first that reads as a name table.
    // Which size is right is settled here, by how much of the rest of the file it explains.
    int best_type = -1;
    int best_record = 0;
    std::vector<MetadataRange> best_ranges;
    for (const auto& [index, candidates] : name_tables) {
        for (const auto& table : candidates) {
            // Deliberately not "the one carrying two names". A type usually carries a name
            // and a namespace in the first two slots, but one build in the corpus does not,
            // and requiring it threw that file away for the wrong reason. How much of the
            // rest of the file a candidate explains is the better test, and it is the only
            // one used here.
            if (table.record < 40 || table.count < 64) continue;
            auto ranges = FindRanges(file, layout.spans, index, table.record);
            if (ranges.size() > best_ranges.size()) {
                best_type = index;
                best_record = table.record;
                best_ranges = std::move(ranges);
            }
        }
    }
    if (best_type < 0 || best_ranges.empty())
        return Error{std::format(
            "the header reads cleanly ({} spans) and the identifier blob is there, but no span "
            "declares ranges that partition another table, so the type record could not be "
            "confirmed. {} span(s) read as record tables. A build that scrambles its tables "
            "looks exactly like this, and guessing past it would produce a dump that is "
            "quietly wrong rather than absent",
            layout.spans.size(), name_tables.size()), 6};

    layout.tables.types = best_type;
    layout.type_record  = best_record;
    layout.ranges       = std::move(best_ranges);
    layout.evidence.push_back(std::format(
        "span {} is the type table: {} records of {} bytes, declaring {} ranges that "
        "partition another table exactly",
        layout.tables.types, layout.Span(best_type).size / static_cast<std::uint32_t>(best_record),
        best_record, layout.ranges.size()));

    // The name and namespace slots come from discovery rather than being assumed to be the
    // first two, because one build in the corpus does not put them there.
    if (const auto* chosen = Primary(name_tables, layout.tables.types)) {
        for (const auto& candidate : name_tables.at(layout.tables.types)) {
            if (candidate.record != layout.type_record) continue;
            layout.name_slot = candidate.name_slots.front();
            layout.namespace_slot =
                candidate.name_slots.size() > 1 ? candidate.name_slots[1] : -1;
            break;
        }
        (void)chosen;
    }
    layout.evidence.push_back(std::format(
        "type name is slot {}, namespace {}", layout.name_slot,
        layout.namespace_slot < 0 ? std::string("not present")
                                  : std::to_string(layout.namespace_slot)));

    // --- naming the tables the ranges point at ---------------------------------------
    //
    // A type's ranges reach its fields, methods, events, properties, nested types,
    // interfaces and vtable. Which is which comes from the target's own shape, not from the
    // slot order, because the slot order moves between versions.
    //
    // Methods are the one target that itself declares a range, into the parameter table.
    // That is distinctive enough to name it outright, and naming it names the parameters too.
    for (const auto& range : layout.ranges) {
        // The method record does not always begin with a name -- one build in the corpus
        // puts something else first -- so the record size can come from the range that found
        // the table rather than from it reading as a name table.
        std::vector<int> sizes;
        if (const auto it = name_tables.find(range.target); it != name_tables.end())
            for (const auto& candidate : it->second) sizes.push_back(candidate.record);
        if (int derived = 0; SpanHolds(layout.Span(range.target), range.total, derived))
            sizes.push_back(derived);

        for (const int record : sizes) {
            for (const auto& reach : FindRanges(file, layout.spans, range.target, record)) {
                const auto* param = Primary(name_tables, reach.target);
                if (!param || param->leading() != 1) continue;
                layout.tables.methods    = range.target;
                layout.method_record     = record;
                layout.tables.parameters = reach.target;
                layout.parameter_record  = param->record;
                break;
            }
            if (layout.tables.methods >= 0) break;
        }
        if (layout.tables.methods >= 0) break;
    }

    // Fields, properties and events are the remaining name tables a type reaches, told apart
    // by record size: a field is a name, a type and a token and nothing else, so it is the
    // narrowest of the three.
    std::vector<std::pair<int, int>> remaining;      // (record size, span)
    for (const auto& range : layout.ranges) {
        if (range.target == layout.tables.methods) continue;
        const auto* table = Primary(name_tables, range.target);
        if (!table || table->leading() != 1) continue;
        remaining.emplace_back(table->record, range.target);
    }
    std::sort(remaining.begin(), remaining.end());
    if (!remaining.empty()) {
        layout.tables.fields = remaining.front().second;
        layout.field_record  = remaining.front().first;
    }
    if (remaining.size() > 1) layout.tables.properties = remaining[1].second;
    if (remaining.size() > 2) layout.tables.events     = remaining[2].second;

    // Nested types: the range target made of bare int32s that are all valid type indices.
    // Needed because a nested type's name is only unique inside the type that declares it,
    // and the live walk builds those names outer-first. Two readings that disagree about a
    // type's name cannot be merged, so this is not cosmetic.
    for (const auto& range : layout.ranges) {
        if (range.target == layout.tables.fields || range.target == layout.tables.methods ||
            range.target == layout.tables.properties || range.target == layout.tables.events)
            continue;

        const auto& span = layout.Span(range.target);
        if (range.total == 0 || span.size / range.total != 4) continue;

        const std::uint32_t types =
            layout.Span(layout.tables.types).size / static_cast<std::uint32_t>(layout.type_record);
        bool all = true;
        for (std::uint32_t i = 0; i < range.total && all; ++i) {
            const auto value = ReadI32(file, span.offset + static_cast<std::size_t>(i) * 4);
            if (value < 0 || static_cast<std::uint32_t>(value) >= types) all = false;
        }
        if (!all) continue;

        layout.tables.nested_types = range.target;
        break;
    }

    // Images declare a range into the type table itself, which nothing else does.
    for (const auto& [index, candidates] : name_tables) {
        if (index == layout.tables.types) continue;
        for (const auto& table : candidates) {
            for (const auto& reach : FindRanges(file, layout.spans, index, table.record)) {
                if (reach.target != layout.tables.types) continue;
                layout.tables.images    = index;
                layout.image_record     = table.record;
                layout.image_types      = reach;
                layout.image_name_slot  = table.name_slots.front();
                break;
            }
            if (layout.tables.images >= 0) break;
        }
        if (layout.tables.images >= 0) break;
    }

    // Token columns. ECMA-335 II.22 puts the table id in the top byte of a metadata token,
    // so a column of them is the one where every value carries the same expected id. That is
    // a CLI fact rather than a Unity one, and it does not move between metadata versions.
    const auto token_slot = [&](int span_index, int record, std::uint8_t table) {
        if (span_index < 0 || record <= 0) return -1;
        const auto& span = layout.Span(span_index);
        const std::uint32_t count = span.size / static_cast<std::uint32_t>(record);
        if (count < kMinRecords) return -1;

        for (int slot = 0; slot < record / 4; ++slot) {
            bool all = true;
            for (const std::uint32_t row : ProbeRows(count)) {
                const auto value = static_cast<std::uint32_t>(ReadI32(
                    file, span.offset + static_cast<std::size_t>(row) * record +
                              static_cast<std::size_t>(slot) * 4));
                if ((value >> 24) != table || (value & 0x00FFFFFFu) == 0) {
                    all = false;
                    break;
                }
            }
            if (all) return slot;
        }
        return -1;
    };

    layout.type_token_slot   = token_slot(layout.tables.types, layout.type_record, 0x02);
    layout.field_token_slot  = token_slot(layout.tables.fields, layout.field_record, 0x04);
    layout.method_token_slot = token_slot(layout.tables.methods, layout.method_record, 0x06);
    layout.evidence.push_back(std::format(
        "metadata tokens: type slot {}, field slot {}, method slot {}",
        layout.type_token_slot, layout.field_token_slot, layout.method_token_slot));

    layout.evidence.push_back(std::format(
        "tables named by shape: strings {}, types {}, images {}, fields {}, methods {}, "
        "parameters {}, properties {}, events {}, nested types {}",
        layout.tables.strings, layout.tables.types, layout.tables.images, layout.tables.fields,
        layout.tables.methods, layout.tables.parameters, layout.tables.properties,
        layout.tables.events, layout.tables.nested_types));

    return layout;
}

std::string MetadataString(std::span<const std::uint8_t> file, const MetadataLayout& layout,
                           std::int32_t index) {
    if (layout.tables.strings < 0 || index < 0) return {};
    const auto& blob = layout.Span(layout.tables.strings);
    if (static_cast<std::uint32_t>(index) >= blob.size) return {};

    const std::size_t begin = blob.offset + static_cast<std::uint32_t>(index);
    std::size_t end = begin;
    const std::size_t limit = blob.offset + blob.size;
    while (end < limit && file[end] != 0) ++end;
    return std::string(reinterpret_cast<const char*>(file.data() + begin), end - begin);
}

} // namespace zircon::il2cpp
