// Emitter tests. Mostly .usmap - it's binary and has the most to get wrong - plus
// reclass, binja, frida_js and python_stubs.
//
// Assertions are about what's on disk, parsed back with a reader written here instead of
// diffed against a golden blob. A golden file tells you the output changed. This tells
// you which field is wrong.

#include "emit/Emitter.h"
#include "ir/Model.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace zircon;

namespace {

int g_failures = 0;
int g_checks   = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expression);
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

// EPropertyType values the emitter writes. Duplicated here on purpose: if the emitter's
// table is edited, this one does not follow, and the mismatch fails the test instead of
// quietly agreeing with a regression.
constexpr std::uint8_t kByte      = 0;
constexpr std::uint8_t kBool      = 1;
constexpr std::uint8_t kInt       = 2;
constexpr std::uint8_t kObject    = 4;
constexpr std::uint8_t kName      = 5;
constexpr std::uint8_t kArray     = 8;
constexpr std::uint8_t kStruct    = 9;
constexpr std::uint8_t kMap       = 24;
constexpr std::uint8_t kEnum      = 26;

class Reader {
public:
    explicit Reader(std::string data) : data_(std::move(data)) {}

    std::uint8_t U8() {
        if (pos_ + 1 > data_.size()) { overran_ = true; return 0; }
        return static_cast<std::uint8_t>(data_[pos_++]);
    }

    std::uint16_t U16() {
        const std::uint16_t low  = U8();
        const std::uint16_t high = U8();
        return static_cast<std::uint16_t>(low | (high << 8));
    }

    std::uint32_t U32() {
        const std::uint32_t low  = U16();
        const std::uint32_t high = U16();
        return low | (high << 16);
    }

    std::string Bytes(std::size_t count) {
        if (pos_ + count > data_.size()) { overran_ = true; return {}; }
        std::string out = data_.substr(pos_, count);
        pos_ += count;
        return out;
    }

    std::size_t Position() const { return pos_; }
    std::size_t Size() const { return data_.size(); }
    bool Overran() const { return overran_; }

private:
    std::string data_;
    std::size_t pos_{0};
    bool        overran_{false};
};

struct ParsedType {
    std::uint8_t            type{0};
    std::uint32_t           name_index{0xFFFFFFFFu};   // struct or enum reference
    std::vector<ParsedType> inner;
};

struct ParsedProperty {
    std::uint16_t schema_index{0};
    std::uint8_t  array_dim{0};
    std::uint32_t name_index{0};
    ParsedType    type;
};

struct ParsedStruct {
    std::uint32_t              name_index{0};
    std::uint32_t              super_index{0};
    std::uint16_t              prop_count{0};
    std::uint16_t              serializable_count{0};
    std::vector<ParsedProperty> properties;
};

struct ParsedEnum {
    std::uint32_t              name_index{0};
    std::vector<std::uint32_t> values;
};

struct ParsedUsmap {
    std::uint16_t magic{0};
    std::uint8_t  version{0};
    std::uint8_t  compression{0};
    std::uint32_t compressed_size{0};
    std::uint32_t decompressed_size{0};

    std::vector<std::string>  names;
    std::vector<ParsedEnum>   enums;
    std::vector<ParsedStruct> structs;

    std::size_t body_bytes{0};
    bool        overran{false};

    const std::string& Name(std::uint32_t index) const {
        static const std::string kMissing = "<out of range>";
        return index < names.size() ? names[index] : kMissing;
    }
};

ParsedType ReadType(Reader& reader, int depth = 0) {
    ParsedType type;
    if (depth > 24) return type;   // the test must not recurse away on a malformed file

    type.type = reader.U8();
    switch (type.type) {
        case kEnum:
            type.inner.push_back(ReadType(reader, depth + 1));
            type.name_index = reader.U32();
            break;
        case kStruct:
            type.name_index = reader.U32();
            break;
        case kArray:
        case 25:   // Set
        case 28:   // Optional
            type.inner.push_back(ReadType(reader, depth + 1));
            break;
        case kMap:
            type.inner.push_back(ReadType(reader, depth + 1));
            type.inner.push_back(ReadType(reader, depth + 1));
            break;
        default:
            break;
    }
    return type;
}

ParsedUsmap Parse(const std::string& bytes) {
    ParsedUsmap out;
    Reader reader(bytes);

    out.magic             = reader.U16();
    out.version           = reader.U8();
    out.compression       = reader.U8();
    out.compressed_size   = reader.U32();
    out.decompressed_size = reader.U32();

    const std::size_t body_start = reader.Position();
    out.body_bytes = bytes.size() - body_start;

    const std::uint32_t name_count = reader.U32();
    for (std::uint32_t i = 0; i < name_count && !reader.Overran(); ++i) {
        const std::uint8_t length = reader.U8();
        out.names.push_back(reader.Bytes(length));
    }

    const std::uint32_t enum_count = reader.U32();
    for (std::uint32_t i = 0; i < enum_count && !reader.Overran(); ++i) {
        ParsedEnum record;
        record.name_index = reader.U32();
        const std::uint8_t values = reader.U8();
        for (std::uint8_t v = 0; v < values; ++v) record.values.push_back(reader.U32());
        out.enums.push_back(std::move(record));
    }

    const std::uint32_t struct_count = reader.U32();
    for (std::uint32_t i = 0; i < struct_count && !reader.Overran(); ++i) {
        ParsedStruct record;
        record.name_index        = reader.U32();
        record.super_index       = reader.U32();
        record.prop_count        = reader.U16();
        record.serializable_count = reader.U16();

        for (std::uint16_t p = 0; p < record.serializable_count && !reader.Overran(); ++p) {
            ParsedProperty property;
            property.schema_index = reader.U16();
            property.array_dim    = reader.U8();
            property.name_index   = reader.U32();
            property.type         = ReadType(reader);
            record.properties.push_back(std::move(property));
        }
        out.structs.push_back(std::move(record));
    }

    // Everything in the file must be consumed. A trailing byte means a field was written
    // that this reader does not know about, which is exactly the bug worth catching.
    out.overran = reader.Overran() || reader.Position() != bytes.size();
    return out;
}

std::string ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

ir::TypeRef Primitive(ir::TypeKind kind, const char* raw, std::int32_t size) {
    ir::TypeRef type;
    type.kind = kind;
    type.raw  = raw;
    type.size = size;
    return type;
}

// A dump holding the shapes that have historically been easy to serialise wrongly:
// a nested container, an inline array, an enum, a struct reference, and a super chain.
ir::Dump MakeDump() {
    ir::Dump dump;
    dump.header.source.process = "Game-Win64-Shipping.exe";

    ir::Package package;
    package.name = "/Script/Engine";

    ir::Enum movement;
    movement.name = "EMovementMode";
    movement.path = "/Script/Engine.EMovementMode";
    movement.values = {{"MOVE_None", 0}, {"MOVE_Walking", 1}, {"MOVE_Falling", 3}};
    package.enums.push_back(movement);

    ir::Struct base;
    base.name     = "Object";
    base.path     = "/Script/CoreUObject.Object";
    base.is_class = true;
    base.size     = 40;

    ir::Struct actor;
    actor.name     = "Actor";
    actor.path     = "/Script/Engine.Actor";
    actor.super    = "/Script/CoreUObject.Object";
    actor.is_class = true;
    actor.size     = 680;

    // TMap<FName, TArray<FVector>> — the nesting the type tree has to reproduce exactly.
    ir::TypeRef vector_struct;
    vector_struct.kind = ir::TypeKind::Struct;
    vector_struct.raw  = "StructProperty";
    vector_struct.name = "/Script/CoreUObject.Vector";
    vector_struct.size = 24;

    ir::TypeRef inner_array;
    inner_array.kind = ir::TypeKind::Array;
    inner_array.raw  = "ArrayProperty";
    inner_array.size = 16;
    inner_array.params.push_back(vector_struct);

    ir::TypeRef map;
    map.kind = ir::TypeKind::Map;
    map.raw  = "MapProperty";
    map.size = 80;
    map.params.push_back(Primitive(ir::TypeKind::Name, "NameProperty", 8));
    map.params.push_back(inner_array);

    ir::Property nested;
    nested.name   = "NestedMap";
    nested.type   = map;
    nested.offset = 40;
    nested.size   = 80;
    actor.properties.push_back(nested);

    // An inline array: the schema index must advance by its dimension, not by one.
    ir::Property inline_array;
    inline_array.name      = "Slots";
    inline_array.type      = Primitive(ir::TypeKind::Int32, "IntProperty", 4);
    inline_array.offset    = 120;
    inline_array.size      = 16;
    inline_array.array_dim = 4;
    actor.properties.push_back(inline_array);

    ir::TypeRef enum_type;
    enum_type.kind = ir::TypeKind::Enum;
    enum_type.raw  = "EnumProperty";
    enum_type.name = "/Script/Engine.EMovementMode";
    enum_type.size = 1;
    enum_type.params.push_back(Primitive(ir::TypeKind::UInt8, "ByteProperty", 1));

    ir::Property mode;
    mode.name   = "MovementMode";
    mode.type   = enum_type;
    mode.offset = 140;
    mode.size   = 1;
    actor.properties.push_back(mode);

    ir::Property owner;
    owner.name = "Owner";
    owner.type = Primitive(ir::TypeKind::ObjectPtr, "ObjectProperty", 8);
    owner.type.name = "/Script/Engine.Actor";
    owner.offset = 148;
    owner.size   = 8;
    actor.properties.push_back(owner);

    ir::Property flag;
    flag.name        = "bHidden";
    flag.type        = Primitive(ir::TypeKind::Bool, "BoolProperty", 1);
    flag.offset      = 156;
    flag.size        = 1;
    flag.is_bitfield = true;
    flag.field_mask  = 0x02;
    flag.bit_index   = 1;
    actor.properties.push_back(flag);

    package.classes.push_back(base);
    package.classes.push_back(actor);
    dump.packages.push_back(package);
    return dump;
}

std::filesystem::path TempDir() {
    return std::filesystem::temp_directory_path() / "zircon_usmap_test";
}

const ParsedStruct* FindStruct(const ParsedUsmap& map, std::string_view name) {
    for (const auto& record : map.structs)
        if (map.Name(record.name_index) == name) return &record;
    return nullptr;
}

const ParsedProperty* FindProperty(const ParsedUsmap& map, const ParsedStruct& record,
                                   std::string_view name) {
    for (const auto& property : record.properties)
        if (map.Name(property.name_index) == name) return &property;
    return nullptr;
}

// ---------------------------------------------------------------------------------

void TestHeaderAndSizes(const ParsedUsmap& map, const std::string& raw) {
    CHECK(map.magic == 0x30C4);
    CHECK(map.version == 0);
    CHECK(map.compression == 0);

    // Both size fields must carry the real payload length. A zero in either truncates the
    // file to nothing in a reader that allocates from one and bounds with the other.
    CHECK(map.compressed_size == map.decompressed_size);
    CHECK(map.compressed_size == map.body_bytes);
    CHECK(map.body_bytes == raw.size() - 12);   // 2 + 1 + 1 + 4 + 4 of header
    CHECK(map.compressed_size > 0);

    // Nothing left over: every byte written is a byte this reader understands.
    CHECK(!map.overran);
}

void TestNameTable(const ParsedUsmap& map) {
    CHECK(!map.names.empty());

    const auto has = [&](std::string_view wanted) {
        for (const auto& name : map.names)
            if (name == wanted) return true;
        return false;
    };

    // Types are referenced by leaf name: the format is flat and carries no packages.
    CHECK(has("Actor"));
    CHECK(has("Object"));
    CHECK(has("Vector"));
    CHECK(has("EMovementMode"));
    CHECK(has("NestedMap"));
    CHECK(has("MOVE_Walking"));
    CHECK(!has("/Script/Engine.Actor"));

    // Deduplicated: "Actor" is the class name and the target of Owner, and must appear
    // once, or readers would treat the two entries as unrelated types.
    int actor_entries = 0;
    for (const auto& name : map.names)
        if (name == "Actor") ++actor_entries;
    CHECK(actor_entries == 1);
}

void TestEnums(const ParsedUsmap& map) {
    CHECK(map.enums.size() == 1);
    if (map.enums.empty()) return;

    const auto& record = map.enums.front();
    CHECK(map.Name(record.name_index) == "EMovementMode");
    CHECK(record.values.size() == 3);
    if (record.values.size() == 3) {
        CHECK(map.Name(record.values[0]) == "MOVE_None");
        CHECK(map.Name(record.values[1]) == "MOVE_Walking");
        CHECK(map.Name(record.values[2]) == "MOVE_Falling");
    }
}

void TestStructs(const ParsedUsmap& map) {
    CHECK(map.structs.size() == 2);

    const ParsedStruct* object = FindStruct(map, "Object");
    CHECK(object != nullptr);
    if (object) {
        // No super must be the sentinel, not index 0, which is a real name.
        CHECK(object->super_index == 0xFFFFFFFFu);
        CHECK(object->serializable_count == 0);
        CHECK(object->prop_count == 0);
    }

    const ParsedStruct* actor = FindStruct(map, "Actor");
    CHECK(actor != nullptr);
    if (!actor) return;

    CHECK(map.Name(actor->super_index) == "Object");
    CHECK(actor->serializable_count == 5);

    // Slot count expands inline arrays: 1 + 4 + 1 + 1 + 1.
    CHECK(actor->prop_count == 8);
}

void TestSchemaIndices(const ParsedUsmap& map) {
    const ParsedStruct* actor = FindStruct(map, "Actor");
    CHECK(actor != nullptr);
    if (!actor || actor->properties.size() != 5) return;

    // The index advances by the array dimension, so the property after a [4] array starts
    // four slots later. Advancing by one would make every later property read the wrong
    // bytes out of an asset.
    CHECK(actor->properties[0].schema_index == 0);   // NestedMap
    CHECK(actor->properties[1].schema_index == 1);   // Slots[4]
    CHECK(actor->properties[1].array_dim == 4);
    CHECK(actor->properties[2].schema_index == 5);   // MovementMode
    CHECK(actor->properties[3].schema_index == 6);   // Owner
    CHECK(actor->properties[4].schema_index == 7);   // bHidden

    for (const auto& property : actor->properties)
        CHECK(property.array_dim >= 1);
}

void TestNestedTypeTree(const ParsedUsmap& map) {
    const ParsedStruct* actor = FindStruct(map, "Actor");
    CHECK(actor != nullptr);
    if (!actor) return;

    const ParsedProperty* nested = FindProperty(map, *actor, "NestedMap");
    CHECK(nested != nullptr);
    if (!nested) return;

    // TMap<FName, TArray<FVector>>
    CHECK(nested->type.type == kMap);
    CHECK(nested->type.inner.size() == 2);
    if (nested->type.inner.size() != 2) return;

    CHECK(nested->type.inner[0].type == kName);

    const ParsedType& value = nested->type.inner[1];
    CHECK(value.type == kArray);
    CHECK(value.inner.size() == 1);
    if (value.inner.empty()) return;

    CHECK(value.inner[0].type == kStruct);
    CHECK(map.Name(value.inner[0].name_index) == "Vector");
}

void TestLeafTypes(const ParsedUsmap& map) {
    const ParsedStruct* actor = FindStruct(map, "Actor");
    if (!actor) return;

    const ParsedProperty* slots = FindProperty(map, *actor, "Slots");
    CHECK(slots && slots->type.type == kInt);

    const ParsedProperty* owner = FindProperty(map, *actor, "Owner");
    // Object references carry no payload in version 0; the tree must stop at the tag.
    CHECK(owner && owner->type.type == kObject);
    CHECK(owner && owner->type.inner.empty());

    const ParsedProperty* flag = FindProperty(map, *actor, "bHidden");
    CHECK(flag && flag->type.type == kBool);

    const ParsedProperty* mode = FindProperty(map, *actor, "MovementMode");
    CHECK(mode && mode->type.type == kEnum);
    if (mode) {
        // An enum writes its storage type first, then the enum name.
        CHECK(mode->type.inner.size() == 1);
        if (!mode->type.inner.empty()) CHECK(mode->type.inner[0].type == kByte);
        CHECK(map.Name(mode->type.name_index) == "EMovementMode");
    }
}

void TestPartialRefused() {
    ir::Dump dump = MakeDump();
    dump.header.partial = true;

    emit::EmitOptions options;
    options.out_dir = TempDir().string();

    const auto* emitter = emit::FindEmitter("usmap");
    CHECK(emitter != nullptr);
    if (!emitter) return;

    // A partial dump has no object data to map. Emitting anyway would produce a file that
    // looks valid and describes nothing at all.
    const auto refused = emitter->emit(dump, options);
    CHECK(!refused.ok());
    CHECK(refused.files.empty());

    options.allow_partial = true;
    const auto allowed = emitter->emit(dump, options);
    CHECK(allowed.ok());
}

void TestUnknownTypeWarns() {
    ir::Dump dump;
    dump.header.source.process = "Game.exe";

    ir::Package package;
    package.name = "/Script/Test";

    ir::Struct record;
    record.name     = "Thing";
    record.path     = "/Script/Test.Thing";
    record.is_class = true;
    record.size     = 16;

    ir::Property mystery;
    mystery.name      = "Mystery";
    mystery.type.kind = ir::TypeKind::Unknown;
    mystery.type.raw  = "SomeFutureProperty";
    mystery.offset    = 0;
    mystery.size      = 8;
    record.properties.push_back(mystery);

    // An unresolved container element must still emit a placeholder: the format is
    // positional, so writing nothing would shift every byte after it.
    ir::Property headless;
    headless.name      = "HeadlessArray";
    headless.type.kind = ir::TypeKind::Array;
    headless.type.raw  = "ArrayProperty";
    headless.offset    = 8;
    headless.size      = 16;
    record.properties.push_back(headless);

    package.classes.push_back(record);
    dump.packages.push_back(package);

    emit::EmitOptions options;
    options.out_dir = TempDir().string();

    const auto* emitter = emit::FindEmitter("usmap");
    if (!emitter) return;

    const auto result = emitter->emit(dump, options);
    CHECK(result.ok());

    // Two problems, two warnings. Never quietly wrong.
    CHECK(result.warnings.size() >= 2);

    bool named_the_type = false;
    for (const auto& warning : result.warnings)
        if (warning.find("SomeFutureProperty") != std::string::npos) named_the_type = true;
    CHECK(named_the_type);

    if (!result.files.empty()) {
        const ParsedUsmap map = Parse(ReadWholeFile(result.files.front()));
        CHECK(!map.overran);

        const ParsedStruct* thing = FindStruct(map, "Thing");
        CHECK(thing != nullptr);
        if (thing) {
            const ParsedProperty* array = FindProperty(map, *thing, "HeadlessArray");
            CHECK(array && array->type.type == kArray);
            CHECK(array && array->type.inner.size() == 1);
        }
    }
}

void TestPackageFilter() {
    ir::Dump dump = MakeDump();

    ir::Package other;
    other.name = "/Script/Other";
    ir::Struct record;
    record.name     = "Excluded";
    record.path     = "/Script/Other.Excluded";
    record.is_class = true;
    record.size     = 8;
    other.classes.push_back(record);
    dump.packages.push_back(other);

    emit::EmitOptions options;
    options.out_dir        = TempDir().string();
    options.package_filter = "/Script/Engine";

    const auto* emitter = emit::FindEmitter("usmap");
    if (!emitter) return;

    const auto result = emitter->emit(dump, options);
    CHECK(result.ok());
    if (result.files.empty()) return;

    const ParsedUsmap map = Parse(ReadWholeFile(result.files.front()));
    CHECK(FindStruct(map, "Actor") != nullptr);
    CHECK(FindStruct(map, "Excluded") == nullptr);
}

void TestRegistered() {
    const auto* emitter = emit::FindEmitter("usmap");
    CHECK(emitter != nullptr);
    CHECK(emitter && emitter->needs_objects);
    CHECK(emit::FindEmitter("reclass") != nullptr);
    CHECK(emit::FindEmitter("not_a_real_emitter") == nullptr);
}

void TestDeterministic() {
    const ir::Dump dump = MakeDump();

    emit::EmitOptions options;
    options.out_dir = TempDir().string();

    const auto* emitter = emit::FindEmitter("usmap");
    if (!emitter) return;

    const auto first = emitter->emit(dump, options);
    CHECK(first.ok());
    if (first.files.empty()) return;
    const std::string a = ReadWholeFile(first.files.front());

    const auto second = emitter->emit(dump, options);
    CHECK(second.ok());
    if (second.files.empty()) return;
    const std::string b = ReadWholeFile(second.files.front());

    // Name-table order comes from traversal order, so an unordered container leaking into
    // the emitter would show up here and nowhere else.
    CHECK(a == b);
    CHECK(!a.empty());
}

void TestReClassEmits() {
    const ir::Dump dump = MakeDump();

    emit::EmitOptions options;
    options.out_dir = TempDir().string();

    const auto* emitter = emit::FindEmitter("reclass");
    CHECK(emitter != nullptr);
    if (!emitter) return;

    const auto result = emitter->emit(dump, options);
    CHECK(result.ok());
    CHECK(result.files.size() == 2);   // loose XML plus the archive
    if (result.files.empty()) return;

    const std::string xml = ReadWholeFile(result.files.front());
    CHECK(xml.find("<reclass") != std::string::npos);
    CHECK(xml.find("AActor") != std::string::npos);
    CHECK(xml.find("NestedMap") != std::string::npos);

    // Offsets only line up if every node consumes exactly its own width, so the emitter
    // pads to the declared size; one that stops short comes out misaligned and mute.
    CHECK(xml.find("tail padding") != std::string::npos);
    CHECK(xml.find("]]>") == std::string::npos);

    if (result.files.size() > 1) {
        const std::string archive = ReadWholeFile(result.files[1]);
        CHECK(archive.size() > 4);
        // Local file header signature, so the archive is at least shaped like a zip.
        CHECK(archive.compare(0, 4, "PK\x03\x04") == 0);
        CHECK(archive.find("data.xml") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------------
// binja
// ---------------------------------------------------------------------------------

// The script's struct table, read back. It's machine-written and the format is fixed, so
// a scanner is a few lines and beats golden text - it says which member is wrong.
struct TableMember {
    int offset{0};
    std::string name;
    char kind{0};
    std::string arg;
    int count{1};
};

struct TableStruct {
    std::string name;
    int size{0};
    int alignment{0};
    std::vector<TableMember> members;
};

std::string Unquote(const std::string& text) {
    if (text.size() < 2 || text.front() != '"') return text;
    return text.substr(1, text.size() - 2);
}

std::vector<std::string> SplitTuple(const std::string& body) {
    // commas inside a quoted field don't separate. nothing nests here, so no depth counter.
    std::vector<std::string> fields;
    std::string current;
    bool in_string = false;
    for (const char c : body) {
        if (c == '"') in_string = !in_string;
        if (c == ',' && !in_string) { fields.push_back(current); current.clear(); continue; }
        current.push_back(c);
    }
    if (!current.empty()) fields.push_back(current);
    return fields;
}

std::vector<TableStruct> ParseBinjaStructs(const std::string& script) {
    std::vector<TableStruct> out;

    const std::size_t table = script.find("ZIRCON_STRUCTS = [");
    if (table == std::string::npos) return out;
    const std::size_t end = script.find("\n]\n", table);

    std::size_t cursor = script.find('\n', table);
    while (cursor != std::string::npos && cursor < end) {
        const std::size_t line_end = script.find('\n', cursor + 1);
        if (line_end == std::string::npos || line_end > end) break;
        const std::string line = script.substr(cursor + 1, line_end - cursor - 1);
        cursor = line_end;
        if (line.size() < 2 || line[0] != '(') continue;

        const std::size_t members_at = line.find(", ((");
        const std::size_t no_members = line.find(", ())");
        const std::size_t split = members_at != std::string::npos ? members_at : no_members;
        if (split == std::string::npos) continue;

        TableStruct record;
        const auto head = SplitTuple(line.substr(1, split - 1));
        if (head.size() < 3) continue;
        record.name = Unquote(head[0]);
        record.size = std::atoi(head[1].c_str());
        record.alignment = std::atoi(head[2].c_str());

        // members are (a),(b),(c), - walk the parenthesised groups
        for (std::size_t i = split; i < line.size(); ++i) {
            if (line[i] != '(') continue;
            const std::size_t close = line.find(')', i);
            if (close == std::string::npos) break;
            const auto fields = SplitTuple(line.substr(i + 1, close - i - 1));
            i = close;
            if (fields.size() != 5) continue;

            TableMember member;
            member.offset = std::atoi(fields[0].c_str());
            member.name   = Unquote(fields[1]);
            const std::string kind = Unquote(fields[2]);
            member.kind   = kind.empty() ? '?' : kind[0];
            member.arg    = Unquote(fields[3]);
            member.count  = std::atoi(fields[4].c_str());
            record.members.push_back(member);
        }
        out.push_back(std::move(record));
    }
    return out;
}

const TableStruct* FindTableStruct(const std::vector<TableStruct>& all,
                                   std::string_view name) {
    for (const auto& record : all) if (record.name == name) return &record;
    return nullptr;
}

void TestBinjaEmits() {
    const ir::Dump dump = MakeDump();

    emit::EmitOptions options;
    options.out_dir = TempDir().string();

    const auto* emitter = emit::FindEmitter("binja");
    CHECK(emitter != nullptr);
    if (!emitter) return;

    const auto result = emitter->emit(dump, options);
    CHECK(result.ok());
    CHECK(result.files.size() == 1);
    if (result.files.empty()) return;

    const std::string script = ReadWholeFile(result.files.front());
    CHECK(script.find("ZIRCON_ENUMS = [") != std::string::npos);
    CHECK(script.find("ZIRCON_FUNCTIONS = [") != std::string::npos);
    CHECK(script.find("def apply_structs(view):") != std::string::npos);

    const auto structs = ParseBinjaStructs(script);
    CHECK(structs.size() >= 3);

    const TableStruct* actor = FindTableStruct(structs, "AActor");
    CHECK(actor != nullptr);
    if (!actor) return;

    CHECK(actor->size == 680);

    // This is the whole point of the format: members have to tile the struct exactly. A gap
    // or an overlap puts every member after it at the wrong address, and nothing in Binary
    // Ninja would tell you.
    int cursor = 0;
    bool tiled = true;
    for (const auto& member : actor->members) {
        if (member.offset != cursor) { tiled = false; break; }
        int width = member.count;
        switch (member.kind) {
            case 'i': case 'u': case 'f': width = std::atoi(member.arg.c_str()) * member.count; break;
            case 'b': width = member.count; break;
            case 'p': width = 8 * member.count; break;
            case 's': {
                const TableStruct* inner = FindTableStruct(structs, member.arg);
                if (!inner) { tiled = false; }
                else width = inner->size * member.count;
                break;
            }
            case 'e': width = 1 * member.count; break;   // the fixture's enum is a byte
            default: tiled = false; break;
        }
        if (!tiled) break;
        cursor += width;
    }
    CHECK(tiled);
    CHECK(cursor == actor->size);

    // inline arrays keep their dimension instead of being flattened into bytes
    bool found_inline_array = false;
    for (const auto& member : actor->members) {
        if (member.name != "Slots") continue;
        found_inline_array = true;
        CHECK(member.kind == 'i');
        CHECK(member.arg == "4");
        CHECK(member.count == 4);
    }
    CHECK(found_inline_array);

    // we don't model TMap, so it should come out as a named placeholder of the right width -
    // never a guess at the internals
    bool found_map = false;
    for (const auto& member : actor->members) {
        if (member.name != "NestedMap") continue;
        found_map = true;
        CHECK(member.kind == 's');
        const TableStruct* placeholder = FindTableStruct(structs, member.arg);
        CHECK(placeholder != nullptr);
        if (placeholder) CHECK(placeholder->size == 80);
    }
    CHECK(found_map);
}

// ---------------------------------------------------------------------------------
// frida_js
// ---------------------------------------------------------------------------------

void TestFridaEmits() {
    const ir::Dump dump = MakeDump();

    emit::EmitOptions options;
    options.out_dir = TempDir().string();

    const auto* emitter = emit::FindEmitter("frida_js");
    CHECK(emitter != nullptr);
    if (!emitter) return;

    const auto result = emitter->emit(dump, options);
    CHECK(result.ok());
    CHECK(result.files.size() == 1);
    if (result.files.empty()) return;

    const std::string script = ReadWholeFile(result.files.front());
    CHECK(script.find("var DATA = JSON.parse(") != std::string::npos);
    CHECK(script.find("wrap: function (address, path)") != std::string::npos);
    CHECK(script.find("\"/Script/Engine.Actor\"") != std::string::npos);
    CHECK(script.find("\"MaxWalkSpeed\"") == std::string::npos);   // not in this fixture
    CHECK(script.find("\"n\":\"Slots\",\"o\":120") != std::string::npos);

    // The blob sits in a single-quoted literal. A raw newline in one is a syntax error and a
    // stray unescaped quote silently truncates the data, so every source line has to close
    // its own literal.
    std::size_t line_start = 0;
    bool balanced = true;
    while (line_start < script.size()) {
        const std::size_t line_end = script.find('\n', line_start);
        const std::string line = script.substr(
            line_start, (line_end == std::string::npos ? script.size() : line_end) - line_start);

        int quotes = 0;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\\') { ++i; continue; }
            if (line[i] == '\'') ++quotes;
        }
        if (line.find("JSON.parse(") != std::string::npos ||
            line.find("' +") != std::string::npos || line.find("');") != std::string::npos) {
            if (quotes % 2 != 0) { balanced = false; break; }
        }
        if (line_end == std::string::npos) break;
        line_start = line_end + 1;
    }
    CHECK(balanced);

    // the mask has to travel with the property. without it the accessor writes a whole byte
    // and clears whatever else is packed in there.
    CHECK(script.find("\"bit\":1,\"mask\":2") != std::string::npos);
}

// ---------------------------------------------------------------------------------
// python_stubs
// ---------------------------------------------------------------------------------

void TestPythonStubsEmit() {
    ir::Dump dump = MakeDump();

    // A keyword enumerator - the most likely way to get a stub that won't parse. `None = 0`
    // is a syntax error, not a name clash.
    for (auto& package : dump.packages)
        for (auto& record : package.enums)
            if (record.name == "EMovementMode") record.values.push_back({"None", 9});

    emit::EmitOptions options;
    options.out_dir = TempDir().string();

    const auto* emitter = emit::FindEmitter("python_stubs");
    CHECK(emitter != nullptr);
    if (!emitter) return;

    const auto result = emitter->emit(dump, options);
    CHECK(result.ok());
    CHECK(result.files.size() >= 3);   // one module, __init__.pyi, py.typed

    std::string engine_stub;
    bool saw_marker = false;
    for (const auto& file : result.files) {
        if (file.find("py.typed") != std::string::npos) saw_marker = true;
        if (file.find("Engine.pyi") != std::string::npos) engine_stub = ReadWholeFile(file);
    }
    CHECK(saw_marker);
    CHECK(!engine_stub.empty());
    if (engine_stub.empty()) return;

    CHECK(engine_stub.find("class AActor(UObject):") != std::string::npos);
    CHECK(engine_stub.find("__size__: ClassVar[int] = 680") != std::string::npos);
    CHECK(engine_stub.find("\"Slots\": 120") != std::string::npos);
    CHECK(engine_stub.find("None_ = 9") != std::string::npos);
    CHECK(engine_stub.find("\n    None = 9") == std::string::npos);

    // an inline array should read as a list of the element type, not the element type
    CHECK(engine_stub.find("Slots: List[int]") != std::string::npos);

    // declared-here-only. inheritance supplies the rest, and repeating a base's offsets in
    // the derived dict would just give them somewhere to drift apart.
    const std::size_t actor_at = engine_stub.find("class AActor(");
    const std::size_t offsets_at = engine_stub.find("__offsets__", actor_at);
    CHECK(offsets_at != std::string::npos);
}

} // namespace

int main() {
    std::error_code ec;
    std::filesystem::create_directories(TempDir(), ec);

    const ir::Dump dump = MakeDump();

    emit::EmitOptions options;
    options.out_dir = TempDir().string();

    const auto* emitter = emit::FindEmitter("usmap");
    if (!emitter) {
        std::fprintf(stderr, "FATAL: usmap emitter is not registered\n");
        return 1;
    }

    const auto result = emitter->emit(dump, options);
    if (!result.ok()) {
        std::fprintf(stderr, "FATAL: emit failed: %s\n", result.error.c_str());
        return 1;
    }
    if (result.files.empty()) {
        std::fprintf(stderr, "FATAL: emit reported no files\n");
        return 1;
    }

    const std::string raw = ReadWholeFile(result.files.front());
    const ParsedUsmap map = Parse(raw);

    TestRegistered();
    TestHeaderAndSizes(map, raw);
    TestNameTable(map);
    TestEnums(map);
    TestStructs(map);
    TestSchemaIndices(map);
    TestNestedTypeTree(map);
    TestLeafTypes(map);
    TestPartialRefused();
    TestUnknownTypeWarns();
    TestPackageFilter();
    TestDeterministic();
    TestReClassEmits();
    TestBinjaEmits();
    TestFridaEmits();
    TestPythonStubsEmit();

    std::filesystem::remove_all(TempDir(), ec);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
