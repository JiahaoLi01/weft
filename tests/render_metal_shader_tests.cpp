#include "constants/render.hpp"
#include "effects/effect_color.hpp"
#include "effects/effect_css.hpp"
#include "effects/effect_parameters.hpp"
#include "effects/effect_repeat.hpp"
#include "effects/effect_textures.hpp"
#include "effects/effect_uv.hpp"
#include "render/backends/metal/shaders.hpp"
#include "support/check.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Metal backend's MSL (cmake/WeftMetal.cmake) as its executor will bind it, checked without Metal: each stage's
// entry point, every resource at the argument table index constants::render::metal names and nothing else bound, the
// sizes of the records and constants the CPU writes, the vertex outputs every fragment input reads, and target px
// mapped to clip space with y down (Metal's clip space is D3D's, so the translation has no y flip). Whether Metal
// compiles and runs the text is checked on Apple devices only.
namespace {
#define CHECK(v) WEFT_REQUIRE(v)

    namespace metal = weft::render::metal;
    namespace slots = weft::constants::render::metal;
    namespace scene = weft::constants::render::scene;

    using weft::testing::require;

    // A struct member or an entry point's parameter: its type, name, array length (0: none) and attributes (the text in
    // [[ ]]).
    struct Field {
        std::string type;
        std::string name;
        std::size_t count{};
        std::string attributes;

        bool operator==(const Field&) const = default;
    };

    struct Entry {
        std::string stage;
        std::string result;
        std::string name;
        std::vector<Field> parameters;
        std::string body;
    };

    // One stage's MSL source: its data structs (by name) and its one entry point.
    struct Stage {
        std::map<std::string, std::vector<Field>> structs;
        Entry entry;
    };

    std::vector<std::string> lines_of(std::string_view text) {
        std::vector<std::string> lines;
        std::string line;
        std::istringstream in{std::string(text)};
        while (std::getline(in, line)) {
            lines.push_back(line);
        }
        return lines;
    }

    // "type name[count] [[attributes]]", with or without the ;
    std::optional<Field> field(std::string text) {
        static const std::regex pattern(R"(^\s*(.*?)\s*\b(\w+)(?:\[(\d+)\])?(?:\s*\[\[(.+)\]\])?;?\s*$)");
        std::smatch m;
        if (!std::regex_match(text, m, pattern) || m[1].str().empty()) {
            return std::nullopt;
        }
        return Field{m[1].str(), m[2].str(), m[3].matched ? std::stoul(m[3].str()) : 0, m[4].str()};
    }

    // Splits a parameter list at its top-level commas.
    std::vector<std::string> split(const std::string& list) {
        std::vector<std::string> parts;
        int depth = 0;
        std::string part;
        for (const char c : list) {
            if (c == '(' || c == '[' || c == '<') {
                ++depth;
            } else if (c == ')' || c == ']' || c == '>') {
                --depth;
            }
            if (c == ',' && depth == 0) {
                parts.push_back(part);
                part.clear();
            } else {
                part += c;
            }
        }
        if (!part.empty()) {
            parts.push_back(part);
        }
        return parts;
    }

    Stage parse(std::string_view text, const std::string& label) {
        require(!text.empty(), label + ": no MSL");
        require(text.find('\r') == std::string_view::npos && text.find('\0') == std::string_view::npos,
                label + ": the text holds a CR or a 0");
        const auto lines = lines_of(text);
        Stage stage;
        static const std::regex entry_pattern(R"(^(vertex|fragment|kernel)\s+(\w+)\s+(\w+)\((.*)\)$)");
        constexpr std::string_view keyword = "struct ";
        int entries = 0;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (lines[i].rfind(keyword, 0) == 0 && i + 1 < lines.size() && lines[i + 1] == "{" &&
                (i == 0 || lines[i - 1].rfind("template", 0) != 0)) {
                const std::string name = lines[i].substr(keyword.size());
                std::vector<Field> members;
                std::size_t j = i + 2;
                for (; j < lines.size() && lines[j] != "};"; ++j) {
                    const auto member = field(lines[j]);
                    require(member && member->type.find('(') == std::string::npos,
                            label + ": struct " + name + " member " + lines[j]);
                    members.push_back(*member);
                }
                stage.structs[name] = members;
                i = j;
            } else if (std::regex_match(lines[i], m, entry_pattern)) {
                ++entries;
                stage.entry.stage = m[1].str();
                stage.entry.result = m[2].str();
                stage.entry.name = m[3].str();
                for (const auto& part : split(m[4].str())) {
                    const auto parameter = field(part);
                    require(parameter.has_value() && !parameter->attributes.empty(),
                            label + ": parameter " + part + " has no attribute");
                    stage.entry.parameters.push_back(*parameter);
                }
                require(i + 1 < lines.size() && lines[i + 1] == "{", label + ": entry body");
                std::size_t j = i + 2;
                for (; j < lines.size() && lines[j] != "}"; ++j) {
                    stage.entry.body += lines[j] + "\n";
                }
                i = j;
            }
        }
        require(entries == 1, label + ": " + std::to_string(entries) + " entry points");
        return stage;
    }

    // MSL sizes and alignments (Metal Shading Language Specification, "Scalar, vector and matrix data types"): what a
    // buffer of the struct holds per element.
    struct Layout {
        std::size_t size{};
        std::size_t align{};
    };

    Layout layout_of(const Stage& stage, const std::string& type) {
        static const std::regex vector(R"(^(packed_)?(float|int|uint)([234])?$)");
        std::smatch m;
        if (std::regex_match(type, m, vector)) {
            const std::size_t n = m[3].matched ? std::stoul(m[3].str()) : 1;
            if (m[1].matched) {
                return {4 * n, 4};
            }
            const std::size_t lanes = n == 3 ? 4 : n;
            return {4 * lanes, 4 * lanes};
        }
        const auto found = stage.structs.find(type);
        require(found != stage.structs.end(), "no layout for type " + type);
        Layout whole{0, 1};
        for (const auto& member : found->second) {
            const auto part = layout_of(stage, member.type);
            const std::size_t stride = (part.size + part.align - 1) / part.align * part.align;
            whole.size = (whole.size + part.align - 1) / part.align * part.align;
            whole.size += member.count ? stride * member.count : part.size;
            whole.align = std::max(whole.align, part.align);
        }
        whole.size = (whole.size + whole.align - 1) / whole.align * whole.align;
        return whole;
    }

    void require_size(const Stage& stage, const std::string& type, int bytes, const std::string& label) {
        if (stage.structs.count(type)) {
            const auto size = layout_of(stage, type).size;
            require(size == std::size_t(bytes), label + ": " + type + " is " + std::to_string(size) +
                                                    " bytes, the CPU writes " + std::to_string(bytes));
        }
    }

    // Where the executor binds each resource of the shared shaders, by name.
    struct Slot {
        std::string kind;
        int index{};
    };

    const std::map<std::string, Slot>& slots_by_name() {
        static const std::map<std::string, Slot> table{
            {"View", {"buffer", slots::buffer_view}},
            {"paints", {"buffer", slots::buffer_records}},
            {"image", {"texture", slots::texture_image}},
            {"clips", {"buffer", slots::buffer_clips}},
            {"order", {"buffer", slots::buffer_order}},
            {"anchors", {"buffer", slots::buffer_anchors}},
            {"linearClamp", {"sampler", slots::sampler_clamp}},
            {"linearRepeat", {"sampler", slots::sampler_repeat}},
            {"effectRepeat", {"sampler", slots::sampler_repeat}},
            {"effectTexture0", {"texture", slots::texture_effects}},
            {"effectTexture1", {"texture", slots::texture_effects + 1}},
            {"effectTexture2", {"texture", slots::texture_effects + 2}},
            {"effectTexture3", {"texture", slots::texture_effects + 3}},
            {"particleSprite", {"texture", slots::texture_effects}},
            {"BackdropPass", {"buffer", slots::backdrop_buffer_constants}},
            {"source", {"texture", slots::backdrop_texture_source}},
            {"mirror", {"sampler", slots::backdrop_sampler_mirror}},
        };
        return table;
    }

    // The entry point's stage and name, each resource at its slot with a type of its kind, the builtins the stage
    // reads, and the required resources among them. Returns the resources by name.
    std::set<std::string> check_entry(const Stage& stage, const std::string& kind,
                                      const std::set<std::string>& required, const std::string& label) {
        require(stage.entry.stage == kind, label + ": a " + stage.entry.stage + " function");
        require(stage.entry.name == (kind == "vertex" ? "vs_main" : "ps_main"), label + ": entry " + stage.entry.name);
        static const std::regex resource(R"(^(buffer|texture|sampler)\((\d+)\)$)");
        const std::set<std::string> builtins = kind == "vertex" ? std::set<std::string>{"vertex_id", "instance_id"}
                                                                : std::set<std::string>{"stage_in", "position"};
        std::set<std::string> found;
        std::map<std::string, std::string> taken;
        for (const auto& parameter : stage.entry.parameters) {
            std::smatch m;
            if (std::regex_match(parameter.attributes, m, resource)) {
                const auto slot = slots_by_name().find(parameter.name);
                require(slot != slots_by_name().end(), label + ": unknown resource " + parameter.name);
                require(m[1].str() == slot->second.kind && std::stoi(m[2].str()) == slot->second.index,
                        label + ": " + parameter.name + " at " + parameter.attributes);
                const auto [held, fresh] = taken.emplace(m[1].str() + " " + m[2].str(), parameter.name);
                require(fresh, label + ": " + parameter.name + " and " + held->second + " share " + held->first);
                const bool typed = slot->second.kind == "buffer"
                                       ? parameter.type.back() == '&' && (parameter.type.rfind("constant ", 0) == 0 ||
                                                                          parameter.type.rfind("const device ", 0) == 0)
                                   : slot->second.kind == "texture" ? parameter.type == "texture2d<float>"
                                                                    : parameter.type == "sampler";
                require(typed, label + ": " + parameter.name + " is a " + parameter.type);
                found.insert(parameter.name);
            } else {
                require(builtins.count(parameter.attributes) != 0,
                        label + ": " + parameter.name + " reads [[" + parameter.attributes + "]]");
            }
        }
        for (const auto& name : required) {
            require(found.count(name) != 0, label + ": does not read " + name);
        }
        return found;
    }

    // A stage struct's members by their attributes.
    std::map<std::string, Field> by_attribute(const Stage& stage, const std::string& type, const std::string& label) {
        const auto found = stage.structs.find(type);
        require(found != stage.structs.end(), label + ": no struct " + type);
        std::map<std::string, Field> members;
        for (const auto& member : found->second) {
            require(!member.attributes.empty(), label + ": " + type + "." + member.name + " has no attribute");
            // The first attribute names the slot; the rest (flat, center_no_perspective) is how it interpolates.
            members[member.attributes.substr(0, member.attributes.find(','))] = member;
        }
        return members;
    }

    // The fragment's color output, and every user input it reads written by the vertex stage with the same type.
    void check_interface(const Stage& vertex, const Stage& fragment, const std::string& label) {
        const auto outputs = by_attribute(vertex, vertex.entry.result, label + " vertex");
        require(outputs.count("position") && outputs.at("position").type == "float4", label + ": no float4 position");
        const auto colors = by_attribute(fragment, fragment.entry.result, label + " fragment");
        require(colors.size() == 1 && colors.count("color(0)") && colors.at("color(0)").type == "float4",
                label + ": not one float4 color(0)");
        std::string inputs;
        for (const auto& parameter : fragment.entry.parameters) {
            if (parameter.attributes == "stage_in") {
                inputs = parameter.type;
            }
        }
        if (inputs.empty()) {
            return;
        }
        for (const auto& [slot, input] : by_attribute(fragment, inputs, label + " fragment")) {
            require(slot.rfind("user(", 0) == 0, label + ": fragment input " + input.name + " at " + slot);
            const auto output = outputs.find(slot);
            require(output != outputs.end(), label + ": the vertex stage writes no " + slot);
            require(output->second.type == input.type && output->second.count == input.count,
                    label + ": " + slot + " is a " + output->second.type + " written, a " + input.type + " read");
        }
        // Structs both stages declare (the records, the constants) are the same.
        for (const auto& [name, members] : vertex.structs) {
            const auto other = fragment.structs.find(name);
            if (other != fragment.structs.end() && name != vertex.entry.result) {
                require(other->second == members, label + ": the stages declare " + name + " differently");
            }
        }
    }

    // Target px to clip space as D3D's and Metal's: y down, x' = 2x - 1 and y' = 1 - 2y over the target, with no
    // component negated after (what -fvk-invert-y or spirv-cross --flip-vert-y would add for Vulkan).
    void check_clip_space(const Stage& vertex, const std::string& label) {
        require(vertex.entry.body.find("* float2(2.0, -2.0)) + float2(-1.0, 1.0)") != std::string::npos,
                label + ": no y-down map into clip space");
        static const std::regex negated(R"(\.y = -)");
        require(!std::regex_search(vertex.entry.body, negated), label + ": a y is negated");
    }

    void SceneProgram() {
        const auto source = metal::scene_shader();
        const auto vertex = parse(source.vertex, "scene vertex");
        const auto fragment = parse(source.fragment, "scene fragment");
        check_entry(vertex, "vertex", {"View", "paints", "order"}, "scene vertex");
        check_entry(fragment, "fragment", {"View", "paints", "image", "clips", "linearClamp", "linearRepeat"},
                    "scene fragment");
        check_interface(vertex, fragment, "scene");
        check_clip_space(vertex, "scene");
        for (const auto* stage : {&vertex, &fragment}) {
            require_size(*stage, "Paint", scene::record_bytes, "scene");
            require_size(*stage, "Clip", scene::clip_bytes, "scene");
            require_size(*stage, "type_View", scene::view_constants_bytes, "scene");
        }
        CHECK(vertex.structs.count("Paint") && fragment.structs.count("Clip") && vertex.structs.count("type_View"));
        // The record order and the anchors are arrays of uint and float4.
        CHECK(layout_of(vertex, "type_StructuredBuffer_uint").size == 4);
        CHECK(layout_of(vertex, "type_StructuredBuffer_v4float").size == 16);
    }

    void ParticleProgram() {
        const auto source = metal::particle_shader();
        const auto vertex = parse(source.vertex, "particle vertex");
        const auto fragment = parse(source.fragment, "particle fragment");
        check_entry(vertex, "vertex", {"View", "paints"}, "particle vertex");
        check_entry(fragment, "fragment", {"particleSprite", "linearClamp"}, "particle fragment");
        check_interface(vertex, fragment, "particles");
        check_clip_space(vertex, "particles");
        require_size(vertex, "Paint", scene::record_bytes, "particles");
        CHECK(vertex.structs.count("Paint"));
        // The particle's index, place across it, color and frame.
        CHECK(by_attribute(vertex, vertex.entry.result, "particles").size() == 5);
    }

    void BackdropPrograms() {
        using weft::render::BackdropFilter;
        for (const auto pass : {BackdropFilter::Area, BackdropFilter::Blur, BackdropFilter::Matrix}) {
            const std::string label = "backdrop " + std::to_string(int(pass));
            const auto source = metal::backdrop_shader(pass);
            const auto vertex = parse(source.vertex, label + " vertex");
            const auto fragment = parse(source.fragment, label + " fragment");
            check_entry(vertex, "vertex", {}, label + " vertex");
            // The area downsample reads texels; the others sample with the mirroring sampler.
            check_entry(fragment, "fragment",
                        pass == BackdropFilter::Area ? std::set<std::string>{"BackdropPass", "source"}
                                                     : std::set<std::string>{"BackdropPass", "source", "mirror"},
                        label + " fragment");
            check_interface(vertex, fragment, label);
            check_clip_space(vertex, label);
            require_size(fragment, "type_BackdropPass", scene::backdrop_constants_bytes, label);
            CHECK(fragment.structs.count("type_BackdropPass"));
        }
        // One fullscreen triangle for all three.
        CHECK(metal::backdrop_shader(BackdropFilter::Area).vertex.data() ==
              metal::backdrop_shader(BackdropFilter::Matrix).vertex.data());
        CHECK(metal::backdrop_shader(BackdropFilter::Blur).fragment !=
              metal::backdrop_shader(BackdropFilter::Matrix).fragment);
    }

    // Each effect's encoded fragment after the scene's vertex stage: its textures in the effect slots, at most as many
    // as it declares.
    void EffectPrograms() {
        const auto vertex = parse(metal::scene_shader().vertex, "scene vertex");
        const std::vector<std::shared_ptr<const weft::paint::EffectProgram>> programs{
            weft::effects::effect_uv(),    weft::effects::effect_parameters(), weft::effects::effect_textures(),
            weft::effects::effect_color(), weft::effects::effect_repeat(),     weft::effects::effect_css(),
        };
        std::size_t textures = 0;
        for (const auto& program : programs) {
            const std::string label = "effect " + program->name;
            const auto fragment = parse(program->msl_encoded, label);
            const auto found = check_entry(fragment, "fragment", {"paints"}, label);
            check_interface(vertex, fragment, label);
            require_size(fragment, "Paint", scene::record_bytes, label);
            for (std::size_t k = 0; k < 4; ++k) {
                if (found.count("effectTexture" + std::to_string(k))) {
                    require(k < program->textures, label + ": reads texture " + std::to_string(k));
                    ++textures;
                }
            }
        }
        // effect_textures reads its two, effect_repeat and effect_css one each.
        CHECK(textures == 4);
        CHECK(weft::effects::effect_css()->msl_encoded.find("effectRepeat [[sampler(1)]]") != std::string::npos);
    }

    // The checks refuse what they exist to catch.
    void ChecksRefuse() {
        const auto source = metal::scene_shader();
        const auto expect_refused = [](auto check) {
            try {
                check();
            } catch (const std::exception&) {
                return;
            }
            throw std::runtime_error("a check accepted a broken shader");
        };
        const auto replaced = [](std::string_view text, std::string_view from, std::string_view to) {
            std::string s(text);
            const auto at = s.find(from);
            require(at != std::string::npos, "no " + std::string(from) + " to replace");
            return s.replace(at, from.size(), to);
        };
        // A resource at another index, an unknown resource, a builtin the executor does not provide.
        expect_refused([&] {
            check_entry(parse(replaced(source.vertex, "paints [[buffer(1)]]", "paints [[buffer(2)]]"), "mutant"),
                        "vertex", {}, "mutant");
        });
        expect_refused([&] {
            check_entry(parse(replaced(source.vertex, "order [[buffer(8)]]", "records [[buffer(8)]]"), "mutant"),
                        "vertex", {}, "mutant");
        });
        expect_refused([&] {
            check_entry(parse(replaced(source.vertex, "[[instance_id]]", "[[instance_id]], uint b [[base_instance]]"),
                              "mutant"),
                        "vertex", {}, "mutant");
        });
        // A record grown by a float4, a vertex output of another type, a fragment input nothing writes.
        expect_refused([&] {
            require_size(parse(replaced(source.fragment, "float4 clips;", "float4 clips;\n    float4 more;"), "mutant"),
                         "Paint", scene::record_bytes, "mutant");
        });
        expect_refused([&] {
            check_interface(
                parse(replaced(source.vertex, "uint out_var_TEXCOORD0", "float out_var_TEXCOORD0"), "mutant"),
                parse(source.fragment, "fragment"), "mutant");
        });
        expect_refused([&] {
            check_interface(
                parse(source.vertex, "vertex"),
                parse(replaced(source.fragment, "uint in_var_TEXCOORD0 [[user(locn0)]];",
                               "uint in_var_TEXCOORD0 [[user(locn0)]];\n    float4 in_var_X [[user(locn4)]];"),
                      "mutant"),
                "mutant");
        });
        // The y flip of Vulkan's SPIR-V, as DXC's -fvk-invert-y writes it into the MSL.
        expect_refused([&] {
            const auto vertex = parse(metal::backdrop_shader(weft::render::BackdropFilter::Blur).vertex, "vertex");
            auto flipped = vertex;
            flipped.entry.body = replaced(vertex.entry.body, "    out.gl_Position = float4(", "    float4 p = float4(");
            flipped.entry.body = replaced(flipped.entry.body, "    out.out_var_TEXCOORD0",
                                          "    p.y = -p.y;\n    out.gl_Position = p;\n    out.out_var_TEXCOORD0");
            check_clip_space(flipped, "mutant");
        });
    }
} // namespace

int main() {
    try {
        SceneProgram();
        ParticleProgram();
        BackdropPrograms();
        EffectPrograms();
        ChecksRefuse();
        std::cout << "PASS render metal shaders: entry points, argument table indices, record sizes, stage interfaces, "
                     "clip space (MSL "
                  << slots::msl_version / 10000 << "." << slots::msl_version / 100 % 100
                  << "; not compiled by Metal here)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
