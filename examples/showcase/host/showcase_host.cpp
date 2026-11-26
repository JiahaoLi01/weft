// D3D11 3D Geometry Showcase Host for Weft promotional screenshots.
// Uses only Weft's public C headers (include/weft) + procedural D3D11 3D geometry (4x MSAA):
//   --mode crpg --page <dir> --out <png>
//       Renders a live 3D procedural dark-fantasy relic pedestal & greatsword into an RGBA16F
//       D3D11 Render Target Texture, imports it zero-copy into Weft as -weft-texture(stage3d),
//       and renders the CRPG UI (with 3-level nested tooltips & dice log) over/around it.
//   --mode jrpg --page <dir> --out <png>
//       Renders a stylized 3D geometric anime battle arena (reflective checkerboard floor,
//       twilight moon sky, floating obsidian monoliths, geometric Boss with 8 crystal swords,
//       geometric Hero with glowing katana) and composites multiple Weft-rendered UI panels
//       (with hardware mipmaps + 16x anisotropic filtering) directly as 3D perspective quads
//       in world space with planar floor reflections + a screen-space HUD overlay.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#include <weft/render_backend.h>
#include <weft/render_backend_d3d11.h>
#include <weft/render_external_texture.h>
#include <weft/weft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
    constexpr float kPi = 3.14159265358979323846f;

    struct Failure : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    void check(bool ok, const std::string& msg) {
        if (!ok) {
            throw Failure(msg);
        }
    }

    void check(weft_result r, const char* msg) {
        if (r != WEFT_OK) {
            throw Failure(std::string(msg) + ": " + weft_result_string(r));
        }
    }

    void check(weft_render_result r, const char* msg) {
        if (r != WEFT_RENDER_OK) {
            throw Failure(std::string(msg) + ": render error " + std::to_string(int(r)));
        }
    }

    std::filesystem::path utf8_path(std::string_view s) {
        return std::filesystem::path(std::u8string(s.begin(), s.end()));
    }

    std::string utf8_str(const std::filesystem::path& p) {
        const auto s = p.generic_u8string();
        return std::string(s.begin(), s.end());
    }

    std::string read_text(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        check(bool(in), "cannot read " + p.string());
        return {std::istreambuf_iterator<char>(in), {}};
    }

    // ---- Minimal PNG writer (stored deflate, lossless) ----
    std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
        static const auto table = [] {
            std::array<std::uint32_t, 256> t{};
            for (std::uint32_t n = 0; n < 256; ++n) {
                std::uint32_t c = n;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
                t[n] = c;
            }
            return t;
        }();
        std::uint32_t crc = ~0u;
        for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
        return ~crc;
    }

    void write_png_bgra(const std::filesystem::path& path, unsigned w, unsigned h, const std::vector<std::uint8_t>& bgra) {
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        std::vector<std::uint8_t> raw;
        raw.reserve(std::size_t(h) * (std::size_t(w) * 4 + 1));
        for (unsigned y = 0; y < h; ++y) {
            raw.push_back(0);
            const auto* row = bgra.data() + std::size_t(y) * w * 4;
            for (unsigned x = 0; x < w; ++x) {
                raw.push_back(row[x * 4 + 2]);
                raw.push_back(row[x * 4 + 1]);
                raw.push_back(row[x * 4 + 0]);
                raw.push_back(255);
            }
        }
        std::vector<std::uint8_t> z{0x78, 0x01};
        std::uint32_t a = 1, b = 0;
        for (std::size_t at = 0; at < raw.size() || at == 0;) {
            const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
            const bool last = at + n == raw.size();
            z.push_back(last ? 1 : 0);
            z.push_back(std::uint8_t(n & 0xff));
            z.push_back(std::uint8_t(n >> 8));
            z.push_back(std::uint8_t(~n & 0xff));
            z.push_back(std::uint8_t((~n >> 8) & 0xff));
            z.insert(z.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + n));
            for (std::size_t i = at; i < at + n; ++i) {
                a = (a + raw[i]) % 65521;
                b = (b + a) % 65521;
            }
            at += n;
            if (last) break;
        }
        const std::uint32_t adler = (b << 16) | a;
        for (int s = 24; s >= 0; s -= 8) z.push_back(std::uint8_t(adler >> s));
        std::ofstream out(path, std::ios::binary);
        check(bool(out), "cannot write " + path.string());
        const auto put32 = [&](std::uint32_t v) {
            const std::uint8_t bytes[4]{std::uint8_t(v >> 24), std::uint8_t(v >> 16), std::uint8_t(v >> 8), std::uint8_t(v)};
            out.write(reinterpret_cast<const char*>(bytes), 4);
        };
        const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
            put32(std::uint32_t(data.size()));
            std::vector<std::uint8_t> typed(type, type + 4);
            typed.insert(typed.end(), data.begin(), data.end());
            out.write(reinterpret_cast<const char*>(typed.data()), std::streamsize(typed.size()));
            put32(crc32(typed.data(), typed.size()));
        };
        static const std::uint8_t sig[8]{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        out.write(reinterpret_cast<const char*>(sig), 8);
        std::vector<std::uint8_t> ihdr(13);
        for (int i = 0; i < 4; ++i) {
            ihdr[i] = std::uint8_t(w >> (24 - 8 * i));
            ihdr[4 + i] = std::uint8_t(h >> (24 - 8 * i));
        }
        ihdr[8] = 8;
        ihdr[9] = 6;
        chunk("IHDR", ihdr);
        chunk("IDAT", z);
        chunk("IEND", {});
    }

    // ---- 3D Math (Row-vector v * M, matching HLSL mul(float4(v,1), M) with row_major) ----
    struct Vec3 {
        float x{}, y{}, z{};
    };
    Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    Vec3 norm(Vec3 a) {
        const float l = std::sqrt(dot(a, a));
        return l > 1e-6f ? a * (1.f / l) : Vec3{0, 1, 0};
    }

    struct Mat4 {
        float m[4][4]{};
        static Mat4 identity() {
            Mat4 r{};
            r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.f;
            return r;
        }
        static Mat4 translate(float x, float y, float z) {
            Mat4 r = identity();
            r.m[3][0] = x; r.m[3][1] = y; r.m[3][2] = z;
            return r;
        }
        static Mat4 scale(float x, float y, float z) {
            Mat4 r{};
            r.m[0][0] = x; r.m[1][1] = y; r.m[2][2] = z; r.m[3][3] = 1.f;
            return r;
        }
        static Mat4 rot_x(float a) {
            Mat4 r = identity();
            const float c = std::cos(a), s = std::sin(a);
            r.m[1][1] = c; r.m[1][2] = s; r.m[2][1] = -s; r.m[2][2] = c;
            return r;
        }
        static Mat4 rot_y(float a) {
            Mat4 r = identity();
            const float c = std::cos(a), s = std::sin(a);
            r.m[0][0] = c; r.m[0][2] = -s; r.m[2][0] = s; r.m[2][2] = c;
            return r;
        }
        static Mat4 rot_z(float a) {
            Mat4 r = identity();
            const float c = std::cos(a), s = std::sin(a);
            r.m[0][0] = c; r.m[0][1] = s; r.m[1][0] = -s; r.m[1][1] = c;
            return r;
        }
        static Mat4 look_at_rh(Vec3 eye, Vec3 target, Vec3 up) {
            const Vec3 f = norm(target - eye);
            const Vec3 s = norm(cross(f, up));
            const Vec3 u = cross(s, f);
            Mat4 r = identity();
            r.m[0][0] = s.x; r.m[0][1] = u.x; r.m[0][2] = -f.x;
            r.m[1][0] = s.y; r.m[1][1] = u.y; r.m[1][2] = -f.y;
            r.m[2][0] = s.z; r.m[2][1] = u.z; r.m[2][2] = -f.z;
            r.m[3][0] = -dot(s, eye);
            r.m[3][1] = -dot(u, eye);
            r.m[3][2] = dot(f, eye);
            return r;
        }
        static Mat4 perspective_rh(float fov_y, float aspect, float zn, float zf) {
            const float ys = 1.f / std::tan(fov_y * 0.5f);
            const float xs = ys / aspect;
            Mat4 r{};
            r.m[0][0] = xs;
            r.m[1][1] = ys;
            r.m[2][2] = zf / (zn - zf);
            r.m[2][3] = -1.f;
            r.m[3][2] = (zn * zf) / (zn - zf);
            return r;
        }
    };

    Mat4 operator*(const Mat4& a, const Mat4& b) {
        Mat4 r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    r.m[i][j] += a.m[i][k] * b.m[k][j];
        return r;
    }

    Vec3 xform_point(Vec3 p, const Mat4& m) {
        return {
            p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
            p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
            p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2],
        };
    }

    Vec3 xform_dir(Vec3 p, const Mat4& m) {
        return norm({
            p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0],
            p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1],
            p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2],
        });
    }

    // ---- Procedural 3D Geometry Builder ----
    struct Vertex {
        float px, py, pz;
        float nx, ny, nz;
        float r, g, b, metal;
        float er, eg, eb, rough;
    };

    struct Material {
        Vec3 albedo{0.4f, 0.4f, 0.45f};
        Vec3 emissive{0.f, 0.f, 0.f};
        float metal = 0.2f;
        float rough = 0.45f;
    };

    struct MeshBuilder {
        std::vector<Vertex> verts;

        void tri(Vec3 a, Vec3 b, Vec3 c, const Material& mat, const Mat4& xf) {
            const Vec3 wa = xform_point(a, xf);
            const Vec3 wb = xform_point(b, xf);
            const Vec3 wc = xform_point(c, xf);
            const Vec3 n = norm(cross(wb - wa, wc - wa));
            const auto push = [&](Vec3 p) {
                verts.push_back({p.x, p.y, p.z, n.x, n.y, n.z, mat.albedo.x, mat.albedo.y, mat.albedo.z, mat.metal,
                                 mat.emissive.x, mat.emissive.y, mat.emissive.z, mat.rough});
            };
            push(wa); push(wb); push(wc);
        }

        void tri_smooth(Vec3 a, Vec3 na, Vec3 b, Vec3 nb, Vec3 c, Vec3 nc, const Material& mat, const Mat4& xf) {
            const auto push = [&](Vec3 p, Vec3 n) {
                const Vec3 wp = xform_point(p, xf);
                const Vec3 wn = xform_dir(n, xf);
                verts.push_back({wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, mat.albedo.x, mat.albedo.y, mat.albedo.z, mat.metal,
                                 mat.emissive.x, mat.emissive.y, mat.emissive.z, mat.rough});
            };
            push(a, na); push(b, nb); push(c, nc);
        }

        void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, const Material& mat, const Mat4& xf) {
            tri(a, b, c, mat, xf);
            tri(a, c, d, mat, xf);
        }

        void box(float sx, float sy, float sz, const Material& mat, const Mat4& xf) {
            const float x = sx * 0.5f, y = sy * 0.5f, z = sz * 0.5f;
            quad({-x, -y,  z}, { x, -y,  z}, { x,  y,  z}, {-x,  y,  z}, mat, xf);
            quad({ x, -y, -z}, {-x, -y, -z}, {-x,  y, -z}, { x,  y, -z}, mat, xf);
            quad({ x, -y,  z}, { x, -y, -z}, { x,  y, -z}, { x,  y,  z}, mat, xf);
            quad({-x, -y, -z}, {-x, -y,  z}, {-x,  y,  z}, {-x,  y, -z}, mat, xf);
            quad({-x,  y,  z}, { x,  y,  z}, { x,  y, -z}, {-x,  y, -z}, mat, xf);
            quad({-x, -y, -z}, { x, -y, -z}, { x, -y,  z}, {-x, -y,  z}, mat, xf);
        }

        void prism(int sides, float r_bot, float r_top, float h, const Material& mat, const Mat4& xf) {
            const float y0 = -h * 0.5f, y1 = h * 0.5f;
            for (int i = 0; i < sides; ++i) {
                const float a0 = (2.f * kPi * i) / sides;
                const float a1 = (2.f * kPi * (i + 1)) / sides;
                const Vec3 b0{std::cos(a0) * r_bot, y0, std::sin(a0) * r_bot};
                const Vec3 b1{std::cos(a1) * r_bot, y0, std::sin(a1) * r_bot};
                const Vec3 t0{std::cos(a0) * r_top, y1, std::sin(a0) * r_top};
                const Vec3 t1{std::cos(a1) * r_top, y1, std::sin(a1) * r_top};
                quad(b1, b0, t0, t1, mat, xf);
                if (r_top > 1e-4f) tri({0, y1, 0}, t1, t0, mat, xf);
                if (r_bot > 1e-4f) tri({0, y0, 0}, b0, b1, mat, xf);
            }
        }

        void octahedron(float rx, float ry, float rz, const Material& mat, const Mat4& xf) {
            const Vec3 top{0, ry, 0}, bot{0, -ry, 0};
            const Vec3 ring[4]{{rx, 0, 0}, {0, 0, rz}, {-rx, 0, 0}, {0, 0, -rz}};
            for (int i = 0; i < 4; ++i) {
                const Vec3 p0 = ring[i], p1 = ring[(i + 1) % 4];
                tri(p1, p0, top, mat, xf);
                tri(p0, p1, bot, mat, xf);
            }
        }

        void torus(float major_r, float minor_r, int segs, int tube_segs, const Material& mat, const Mat4& xf) {
            for (int i = 0; i < segs; ++i) {
                const float u0 = (2.f * kPi * i) / segs;
                const float u1 = (2.f * kPi * (i + 1)) / segs;
                for (int j = 0; j < tube_segs; ++j) {
                    const float v0 = (2.f * kPi * j) / tube_segs;
                    const float v1 = (2.f * kPi * (j + 1)) / tube_segs;
                    const auto pt = [&](float u, float v) -> std::pair<Vec3, Vec3> {
                        const float cu = std::cos(u), su = std::sin(u);
                        const float cv = std::cos(v), sv = std::sin(v);
                        const float rr = major_r + minor_r * cv;
                        return {{rr * cu, minor_r * sv, rr * su}, {cv * cu, sv, cv * su}};
                    };
                    const auto [p00, n00] = pt(u0, v0);
                    const auto [p10, n10] = pt(u1, v0);
                    const auto [p11, n11] = pt(u1, v1);
                    const auto [p01, n01] = pt(u0, v1);
                    tri_smooth(p00, n00, p01, n01, p11, n11, mat, xf);
                    tri_smooth(p00, n00, p11, n11, p10, n10, mat, xf);
                }
            }
        }

        // Diamond-section sculpted geometric greatsword blade (base at y=0, tip at y=len)
        void blade(float len, float half_w, float half_t, float tip_len, const Material& mat, const Mat4& xf) {
            const float shoulder = len - tip_len;
            const float w_top = half_w * 0.72f;
            const float t_top = half_t * 0.7f;
            // Base cross section -> shoulder cross section
            const Vec3 b[4]{{0, 0, half_t}, {half_w, 0, 0}, {0, 0, -half_t}, {-half_w, 0, 0}};
            const Vec3 s[4]{{0, shoulder, t_top}, {w_top, shoulder, 0}, {0, shoulder, -t_top}, {-w_top, shoulder, 0}};
            const Vec3 tip{0, len, 0};
            for (int i = 0; i < 4; ++i) {
                const int j = (i + 1) % 4;
                quad(b[i], b[j], s[j], s[i], mat, xf);
                tri(s[i], s[j], tip, mat, xf);
            }
            quad(b[3], b[2], b[1], b[0], mat, xf);
        }

        // Smooth UV sphere for planets/sun (emissive.x can encode planet kind when used with planet shader)
        void sphere(float r, int stacks, int slices, const Material& mat, const Mat4& xf) {
            for (int i = 0; i < stacks; ++i) {
                const float p0 = -kPi * 0.5f + kPi * float(i) / float(stacks);
                const float p1 = -kPi * 0.5f + kPi * float(i + 1) / float(stacks);
                for (int j = 0; j < slices; ++j) {
                    const float t0 = 2.f * kPi * float(j) / float(slices);
                    const float t1 = 2.f * kPi * float(j + 1) / float(slices);
                    const auto nrm = [](float p, float t) -> Vec3 {
                        return {std::cos(p) * std::cos(t), std::sin(p), std::cos(p) * std::sin(t)};
                    };
                    const Vec3 n00 = nrm(p0, t0), n10 = nrm(p1, t0), n11 = nrm(p1, t1), n01 = nrm(p0, t1);
                    tri_smooth(n00 * r, n00, n10 * r, n10, n11 * r, n11, mat, xf);
                    tri_smooth(n00 * r, n00, n11 * r, n11, n01 * r, n01, mat, xf);
                }
            }
        }

        // Flat annular ring disk in XZ plane (for planetary rings like Saturn & orbital selection halos)
        void ring_disk(float r_in, float r_out, int segs, const Material& mat, const Mat4& xf) {
            for (int i = 0; i < segs; ++i) {
                const float a0 = 2.f * kPi * float(i) / float(segs);
                const float a1 = 2.f * kPi * float(i + 1) / float(segs);
                const Vec3 i0{std::cos(a0) * r_in, 0, std::sin(a0) * r_in};
                const Vec3 o0{std::cos(a0) * r_out, 0, std::sin(a0) * r_out};
                const Vec3 o1{std::cos(a1) * r_out, 0, std::sin(a1) * r_out};
                const Vec3 i1{std::cos(a1) * r_in, 0, std::sin(a1) * r_in};
                const Vec3 up{0, 1, 0};
                tri_smooth(i0, up, o0, up, o1, up, mat, xf);
                tri_smooth(i0, up, o1, up, i1, up, mat, xf);
            }
        }
    };

    // ---- HLSL Shaders (compiled via D3DCompile) ----
    constexpr const char* kSceneHlsl = R"(
cbuffer FrameCB : register(b0) {
    row_major float4x4 viewProj;
    float4 eyePos;
    float4 keyDir;      // xyz dir to light, w intensity
    float4 keyColor;    // rgb
    float4 fillDir;     // xyz dir to light, w intensity
    float4 fillColor;   // rgb
    float4 rimColor;    // rgb, w power
    float4 fogColor;    // rgb, w density
    float4 skyBottom;   // rgb, w mode (0=crpg linear HDR, 1=jrpg srgb)
    float4 skyTop;      // rgb, w horizon glow
    float4 floorGlowA;  // xyz pos, w radius
    float4 floorColA;   // rgb, w intensity
    float4 floorGlowB;  // xyz pos, w radius
    float4 floorColB;   // rgb, w intensity
};

float3 aces_tonemap(float3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 encode_output(float3 lin) {
    if (skyBottom.w > 0.5) {
        return pow(aces_tonemap(lin), 1.0 / 2.2);
    }
    return aces_tonemap(lin);
}

// --- Sky Backdrop Fullscreen Triangle ---
struct SkyOut { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };
SkyOut sky_vs(uint v : SV_VertexID) {
    float2 p = float2((v << 1) & 2, v & 2) * 2.0 - 1.0;
    SkyOut o;
    o.pos = float4(p, 0.9999, 1.0);
    o.ndc = p;
    return o;
}
float4 sky_fs(SkyOut i) : SV_TARGET {
    float t = saturate(i.ndc.y * 0.5 + 0.5);
    float3 col = lerp(skyBottom.rgb, skyTop.rgb, pow(t, 0.85));
    if (skyBottom.w < 0.5) {
        // CRPG Modern Obsidian-Zinc Sanctuary subtle brass candlelight & cool rim glow (true linear space)
        float2 p = float2(i.ndc.x * 0.78, i.ndc.y - 0.02);
        float radial = exp(-dot(p, p) * 3.1);
        col += float3(0.11, 0.068, 0.024) * radial * 0.24;
        float2 moon = float2(i.ndc.x * 1.1, i.ndc.y - 0.55);
        col += float3(0.020, 0.032, 0.062) * exp(-dot(moon, moon) * 3.4) * 0.36;
    } else if (skyBottom.w < 1.5) {
        // JRPG Electric Twilight Horizon + Crisp Crescent Moon + Starfield
        float horizon = exp(-abs(i.ndc.y + 0.02) * 5.2);
        col += float3(0.95, 0.10, 0.48) * horizon * 0.46;
        // Crescent Moon positioned upper-center-left
        float2 mp = float2((i.ndc.x + 0.10) * 1.777, i.ndc.y - 0.34);
        float d1 = length(mp);
        float d2 = length(mp - float2(0.11, 0.04));
        float moon_glow = exp(-d1 * 2.9) * 0.38;
        col += float3(0.32, 0.58, 1.0) * moon_glow;
        float moon_disc = smoothstep(0.34, 0.33, d1) * smoothstep(0.29, 0.305, d2);
        col = lerp(col, float3(1.15, 1.32, 1.65), moon_disc);
        // Subtle crisp star sparkles in upper sky
        float2 cell = floor((i.ndc + 1.0) * float2(70.0, 40.0));
        float h = frac(sin(dot(cell, float2(127.1, 311.7))) * 43758.5453);
        float2 f = frac((i.ndc + 1.0) * float2(70.0, 40.0)) - 0.5;
        if (h > 0.965 && i.ndc.y > 0.05) {
            col += float3(0.8, 0.88, 1.0) * smoothstep(0.24, 0.02, length(f)) * saturate(i.ndc.y * 2.0);
        }
    } else if (skyBottom.w < 2.5) {
        // 4X Stellaris Deep Space Galactic Band + Multi-Layer Starfield + Solar Corona Halo
        col = float3(0.004, 0.007, 0.015);
        float2 p = float2(i.ndc.x * 1.777, i.ndc.y);
        // Diagonal subtle sapphire-teal & violet galactic dust band
        float band = exp(-pow((p.y * 0.85 - p.x * 0.32 + 0.06), 2.0) * 2.6);
        float neb = 0.5 + 0.5 * sin(p.x * 3.1 + sin(p.y * 4.2));
        col += float3(0.014, 0.036, 0.072) * band * (0.65 + 0.55 * neb);
        col += float3(0.028, 0.014, 0.045) * band * (1.0 - neb) * 0.55;
        // Blazing Solar Corona Glow around Sun screen position stored in floorGlowA.xy
        float2 sp = p - float2(floorGlowA.x * 1.777, floorGlowA.y);
        float sd = length(sp);
        col += float3(1.25, 0.42, 0.06) * exp(-sd * 4.4) * 0.48;
        col += float3(1.40, 0.82, 0.24) * exp(-sd * 9.5) * 0.75;
        // Subtle Stellaris anamorphic solar flare streak
        col += float3(1.10, 0.74, 0.34) * exp(-abs(sp.y) * 34.0) * exp(-abs(sp.x) * 3.2) * 0.16;
        // Multi-scale crisp procedural starfield
        for (int layer = 0; layer < 2; ++layer) {
            float sc = layer == 0 ? 78.0 : 138.0;
            float2 cell = floor((i.ndc + 1.5) * float2(sc * 1.777, sc));
            float h = frac(sin(dot(cell, float2(127.1 + layer * 41.0, 311.7))) * 43758.5453);
            float2 f = frac((i.ndc + 1.5) * float2(sc * 1.777, sc)) - 0.5;
            if (h > (layer == 0 ? 0.968 : 0.978)) {
                float3 scCol = h > 0.990 ? float3(1.0, 0.84, 0.62) : (h > 0.980 ? float3(0.72, 0.88, 1.0) : float3(0.95, 0.96, 1.0));
                float br = smoothstep(0.24, 0.02, length(f)) * (layer == 0 ? 0.95 : 0.55);
                col += scCol * br;
            }
        }
    } else {
        // FPS 3D Whitebox Clean Studio Daylight Sky + Soft Sun Atmospheric Glow
        float2 p = float2((i.ndc.x + 0.32) * 1.777, i.ndc.y - 0.48);
        float sd = length(p);
        col += float3(0.95, 0.84, 0.64) * exp(-sd * 2.2) * 0.32;
        col += float3(1.0, 0.94, 0.82) * exp(-sd * 9.0) * 0.45;
        float horiz = exp(-abs(i.ndc.y - 0.02) * 4.5);
        col += float3(0.24, 0.32, 0.42) * horiz * 0.28;
    }
    return float4(encode_output(col), 1.0);
}

// --- Reflective Procedural 3D Floor Plane ---
struct FloorOut { float4 pos : SV_POSITION; float3 world : TEXCOORD0; };
FloorOut floor_vs(uint v : SV_VertexID) {
    float2 corners[6] = { float2(0,0), float2(1,0), float2(0,1), float2(0,1), float2(1,0), float2(1,1) };
    float2 q = (corners[v] * 2.0 - 1.0) * 48.0;
    FloorOut o;
    o.world = float3(q.x, 0.0, q.y);
    o.pos = mul(float4(o.world, 1.0), viewProj);
    return o;
}
float4 floor_fs(FloorOut i) : SV_TARGET {
    float dist = length(i.world - eyePos.xyz);
    float fog = 1.0 - exp(-dist * fogColor.w);
    float3 col;
    if (skyBottom.w < 0.5) {
        // Dark carved obsidian stone floor with subtle tile grooves (true linear space)
        float2 p = i.world.xz * 0.9;
        float2 grid = abs(frac(p - 0.5) - 0.5) / max(fwidth(p), 0.001);
        float seam = 1.0 - min(min(grid.x, grid.y), 1.0);
        col = float3(0.0045, 0.0038, 0.0036) * (1.0 - seam * 0.55);
    } else {
        // Stylized Persona / Metaphor glossy checkerboard + subtle neon grid lines
        float2 p = i.world.xz * 0.85;
        float2 fp = floor(p);
        float checker = fmod(abs(fp.x + fp.y), 2.0);
        float2 g = abs(frac(p - 0.5) - 0.5) / max(fwidth(p), 0.001);
        float line_a = 1.0 - min(min(g.x, g.y), 1.0);
        float3 darkTile = float3(0.010, 0.009, 0.024);
        float3 lightTile = float3(0.14, 0.16, 0.27);
        col = lerp(darkTile, lightTile, checker) + float3(0.55, 0.08, 0.38) * line_a * 0.18;
        // Glowing crimson rim line near back of arena
        float backLine = exp(-abs(i.world.z + 7.2) * 4.5);
        col += float3(1.4, 0.08, 0.36) * backLine * 0.65;
    }
    float da = length(i.world.xz - floorGlowA.xz);
    col += floorColA.rgb * (floorColA.w / (1.0 + da * da * floorGlowA.w));
    float db = length(i.world.xz - floorGlowB.xz);
    col += floorColB.rgb * (floorColB.w / (1.0 + db * db * floorGlowB.w));
    col = lerp(col, fogColor.rgb, saturate(fog));
    return float4(encode_output(col), 1.0);
}

// --- Sculpted 3D Geometry Mesh Shader ---
struct MeshIn {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float4 colMetal : COLOR0;
    float4 emiRough : COLOR1;
};
struct MeshOut {
    float4 pos : SV_POSITION;
    float3 world : TEXCOORD0;
    float3 nrm : TEXCOORD1;
    float4 colMetal : COLOR0;
    float4 emiRough : COLOR1;
};
MeshOut mesh_vs(MeshIn i) {
    MeshOut o;
    o.world = i.pos;
    o.nrm = normalize(i.nrm);
    o.pos = mul(float4(i.pos, 1.0), viewProj);
    o.colMetal = i.colMetal;
    o.emiRough = i.emiRough;
    return o;
}
float4 mesh_fs(MeshOut i) : SV_TARGET {
    float3 N = normalize(i.nrm);
    float3 V = normalize(eyePos.xyz - i.world);
    float3 albedo = i.colMetal.rgb;
    float metal = i.colMetal.a;
    float3 emissive = i.emiRough.rgb;
    float rough = clamp(i.emiRough.a, 0.08, 0.95);
    float specPow = lerp(96.0, 10.0, rough);

    float3 L1 = normalize(keyDir.xyz);
    float3 H1 = normalize(L1 + V);
    float ndl1 = saturate(dot(N, L1));
    float spec1 = pow(saturate(dot(N, H1)), specPow);

    float3 L2 = normalize(fillDir.xyz);
    float3 H2 = normalize(L2 + V);
    float ndl2 = saturate(dot(N, L2));
    float spec2 = pow(saturate(dot(N, H2)), specPow * 0.7);

    float3 F0 = lerp(float3(0.05, 0.05, 0.05), albedo, metal);
    float3 diff = albedo * (1.0 - metal * 0.75);
    float3 ambient = albedo * (skyBottom.w < 0.5 ? (0.04 + 0.03 * saturate(N.y * 0.5 + 0.5)) : (0.14 + 0.10 * saturate(N.y * 0.5 + 0.5)));

    float3 lit = ambient;
    lit += (diff * ndl1 + F0 * spec1 * 1.45) * keyColor.rgb * keyDir.w;
    lit += (diff * ndl2 + F0 * spec2 * 1.10) * fillColor.rgb * fillDir.w;

    float fresnel = pow(1.0 - saturate(dot(N, V)), rimColor.w);
    lit += rimColor.rgb * fresnel * (0.55 + 0.65 * metal);
    lit += emissive;

    float dist = length(i.world - eyePos.xyz);
    float fog = 1.0 - exp(-dist * fogColor.w);
    lit = lerp(lit, fogColor.rgb, saturate(fog));
    return float4(encode_output(lit), 1.0);
}

)";
    constexpr const char* kSceneHlslB = R"(
// --- Dedicated 3D Solar System Planet Shader (Earth continents/clouds/city-lights, Sun plasma, Saturn bands) ---
float hash31(float3 p) {
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}
float vnoise(float3 p) {
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash31(i + float3(0,0,0)), n100 = hash31(i + float3(1,0,0));
    float n010 = hash31(i + float3(0,1,0)), n110 = hash31(i + float3(1,1,0));
    float n001 = hash31(i + float3(0,0,1)), n101 = hash31(i + float3(1,0,1));
    float n011 = hash31(i + float3(0,1,1)), n111 = hash31(i + float3(1,1,1));
    float nx00 = lerp(n000, n100, f.x), nx10 = lerp(n010, n110, f.x);
    float nx01 = lerp(n001, n101, f.x), nx11 = lerp(n011, n111, f.x);
    return lerp(lerp(nx00, nx10, f.y), lerp(nx01, nx11, f.y), f.z);
}
float fbm3(float3 p) {
    float v = 0.0, a = 0.5;
    for (int k = 0; k < 5; ++k) {
        v += a * vnoise(p);
        p = p * 2.07 + float3(1.7, 9.2, 4.3);
        a *= 0.5;
    }
    return v;
}

float4 planet_fs(MeshOut i) : SV_TARGET {
    float3 N = normalize(i.nrm);
    float3 V = normalize(eyePos.xyz - i.world);
    float3 sunPos = floorGlowB.xyz;
    float3 L = normalize(sunPos - i.world);
    float ndl = dot(N, L);
    float day = smoothstep(-0.08, 0.22, ndl);
    float diffuse = saturate(ndl);
    float fresnel = pow(1.0 - saturate(dot(N, V)), 3.2);
    float kind = i.colMetal.a; // 0=regular mesh, 1=Sun, 2=Earth, 3=Mars, 4=Saturn, 5=SaturnRing, 6=Moon/Venus

    if (kind > 0.5 && kind < 1.5) {
        // Blazing G2V Solar Plasma Surface (rich orange-amber convection cells + incandescent core)
        float gran = fbm3(N * 6.5 + float3(0.4, 1.2, 0.7));
        float flare = fbm3(N * 14.0 + float3(2.1, 0.3, 4.4));
        float hot = saturate(gran * 1.15 + flare * 0.35 - 0.18);
        float3 core = lerp(float3(0.85, 0.16, 0.015), float3(2.35, 1.18, 0.22), hot);
        core = lerp(core, float3(2.8, 2.25, 1.15), smoothstep(0.68, 0.96, hot));
        float limb = pow(saturate(dot(N, V)), 0.55);
        float3 col = core * (0.48 + 0.72 * limb) + float3(1.65, 0.44, 0.06) * fresnel * 1.35;
        return float4(encode_output(col), 1.0);
    }
    if (kind > 1.5 && kind < 2.5) {
        // Detailed Procedural 3D Earth (Continents, Oceans, Specular Sun Glint, Swirling Clouds, Blue Atmosphere, Night City Lights!)
        float3 p = N * 2.45 + float3(1.35, 0.35, 0.95);
        float elev = fbm3(p);
        float land = smoothstep(0.475, 0.525, elev);
        float mountain = smoothstep(0.62, 0.77, elev);
        float pole = smoothstep(0.79, 0.89, abs(N.y) + (elev - 0.5) * 0.14);
        float3 ocean = lerp(float3(0.014, 0.095, 0.32), float3(0.032, 0.21, 0.48), smoothstep(0.30, 0.49, elev));
        float3 terrain = lerp(float3(0.06, 0.28, 0.095), float3(0.44, 0.35, 0.17), smoothstep(0.54, 0.68, elev));
        terrain = lerp(terrain, float3(0.52, 0.49, 0.45), mountain * 0.65);
        float3 surf = lerp(ocean, terrain, land);
        surf = lerp(surf, float3(0.88, 0.93, 0.98), pole);

        // Swirling cyclone cloud layer
        float3 cp = N * 3.7 + float3(3.4, 1.1, 2.1);
        cp.xz += float2(fbm3(cp * 0.8) - 0.5, fbm3(cp * 0.8 + 4.0) - 0.5) * 0.65;
        float cloud = smoothstep(0.54, 0.73, fbm3(cp));
        surf = lerp(surf, float3(0.92, 0.95, 1.0), cloud * 0.76);

        // Specular ocean glint on day side
        float3 H = normalize(L + V);
        float spec = pow(saturate(dot(N, H)), 42.0) * (1.0 - land) * (1.0 - cloud) * 0.85;
        float wrapDiff = saturate(ndl * 1.12 + 0.06);
        float3 lit = surf * (0.025 + 1.65 * wrapDiff) + float3(1.0, 0.92, 0.74) * spec;

        // Glowing Golden Megalopolis City Lights on Earth's Night Side!
        float night = smoothstep(0.06, -0.24, ndl);
        float cities = smoothstep(0.58, 0.73, fbm3(N * 14.5)) * land * (1.0 - pole) * (1.0 - cloud * 0.65);
        lit += float3(1.65, 0.96, 0.32) * cities * night * 1.35;

        // Crisp Blue Rayleigh Atmospheric Scattering Rim
        float3 atmo = float3(0.20, 0.64, 1.45) * fresnel * (0.38 + 1.55 * day);
        lit += atmo;
        return float4(encode_output(lit), 1.0);
    }
    if (kind > 2.5 && kind < 3.5) {
        // Mars (Rusty Terracotta + Polar Cap + Subtle Thin Orange Atmosphere)
        float n = fbm3(N * 3.8 + float3(2.2, 0.5, 1.1));
        float3 col = lerp(float3(0.34, 0.09, 0.04), float3(0.64, 0.24, 0.10), n);
        float pole = smoothstep(0.84, 0.92, abs(N.y));
        col = lerp(col, float3(0.82, 0.78, 0.74), pole);
        float3 lit = col * (0.02 + 1.4 * diffuse) + float3(0.85, 0.38, 0.16) * fresnel * (0.2 + 0.6 * day);
        return float4(encode_output(lit), 1.0);
    }
    if (kind > 3.5 && kind < 4.5) {
        // Saturn / Gas Giant Horizontal Atmospheric Bands
        float turb = fbm3(N * 3.0) * 0.12;
        float band = 0.5 + 0.5 * sin((N.y + turb) * 18.0);
        float3 col = lerp(float3(0.46, 0.35, 0.20), float3(0.78, 0.66, 0.44), band);
        float3 lit = col * (0.03 + 1.35 * diffuse) + float3(0.95, 0.80, 0.52) * fresnel * (0.15 + 0.55 * day);
        return float4(encode_output(lit), 1.0);
    }
    if (kind > 4.5 && kind < 5.5) {
        // Saturn Concentric Planetary Ring Disk with Cassini Division
        float r = length(i.world - i.emiRough.xyz);
        float cassini = smoothstep(0.018, 0.052, abs(r - 1.15));
        float bands = 0.45 + 0.55 * (0.5 + 0.5 * sin(r * 58.0) * cos(r * 23.0));
        float edge = smoothstep(0.82, 0.90, r) * smoothstep(1.48, 1.38, r);
        float3 col = lerp(float3(0.18, 0.15, 0.11), float3(0.72, 0.62, 0.44), bands * cassini) * (0.25 + 0.75 * edge);
        float3 lit = col * (0.24 + 0.95 * saturate(dot(float3(0, 1, 0), L) * 0.5 + 0.5));
        return float4(encode_output(lit), 1.0);
    }
    // Generic Rocky Planet / Moon / Emissive Orbit Ring
    float3 base = i.colMetal.rgb;
    float3 emi = i.emiRough.rgb;
    float3 lit = base * (0.04 + 1.35 * diffuse) + emi;
    return float4(encode_output(lit), 1.0);
}

// --- Procedural 3D Whitebox / Greybox Prototyping Grid Shader (UE5 / Valorant Dev-Grid Style) ---
float4 whitebox_fs(MeshOut i) : SV_TARGET {
    float3 N = normalize(i.nrm);
    float3 V = normalize(eyePos.xyz - i.world);
    float3 albedo = i.colMetal.rgb;
    float mode = i.colMetal.a; // 0=triplanar dev-grid whitebox, 1=smooth metal/viewmodel, 2=enemy mannequin with crimson rim
    float3 emissive = i.emiRough.rgb;
    float rough = clamp(i.emiRough.a, 0.08, 0.95);

    if (mode < 0.5) {
        // World-space triplanar 1m / 0.5m / 2m prototyping dev-grid with screen-space AA
        float2 uv = abs(N.y) > 0.55 ? i.world.xz : (abs(N.x) > 0.55 ? i.world.zy : i.world.xy);
        float2 tile = floor(uv);
        float checker = fmod(abs(tile.x + tile.y), 2.0);
        albedo *= lerp(0.93, 1.04, checker);

        float2 pSub = uv * 2.0; // 0.5m grid
        float2 gSub = abs(frac(pSub - 0.5) - 0.5) / max(fwidth(pSub), 0.002);
        float lSub = 1.0 - min(min(gSub.x, gSub.y), 1.0);

        float2 pMaj = uv * 0.5; // 2.0m major meter grid
        float2 gMaj = abs(frac(pMaj - 0.5) - 0.5) / max(fwidth(pMaj), 0.002);
        float lMaj = 1.0 - min(min(gMaj.x, gMaj.y), 1.0);

        albedo *= (1.0 - lSub * 0.13 - lMaj * 0.26);
    }

    float3 L1 = normalize(keyDir.xyz);
    float3 H1 = normalize(L1 + V);
    float ndl1 = saturate(dot(N, L1));
    float specPow = lerp(80.0, 12.0, rough);
    float spec1 = pow(saturate(dot(N, H1)), specPow);

    float3 L2 = normalize(fillDir.xyz);
    float ndl2 = saturate(dot(N, L2));

    // Analytic studio sun shadow cast by overhead skybridge + side walls onto the courtyard floor
    float sunVis = 1.0;
    if (mode < 0.5 && i.world.y < 0.10) {
        // Skybridge cast shadow band across courtyard floor
        float br = smoothstep(-16.0, -15.3, i.world.z) * smoothstep(-11.0, -11.7, i.world.z);
        sunVis *= (1.0 - br * 0.68);
        // Left architectural wall cast sun shadow onto left side of floor
        float lw = smoothstep(-2.4, -3.6, i.world.x);
        sunVis *= (1.0 - lw * 0.52);
    }

    // Crisp hemisphere sky/ground ambient + subtle floor-contact AO on vertical surfaces
    float3 hemi = lerp(float3(0.05, 0.07, 0.10), float3(0.19, 0.25, 0.34), saturate(N.y * 0.5 + 0.5));
    float contactAO = (mode < 0.5 && abs(N.y) < 0.5) ? lerp(0.64, 1.0, saturate(i.world.y * 0.90)) : 1.0;

    float metal = (mode > 0.5 && mode < 1.5) ? 0.68 : 0.05;
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    float3 diff = albedo * (1.0 - metal * 0.7);
    float3 lit = diff * hemi * contactAO;
    lit += (diff * ndl1 + F0 * spec1 * 1.15) * keyColor.rgb * keyDir.w * sunVis;
    lit += diff * ndl2 * fillColor.rgb * fillDir.w;

    // Valorant-style crisp enemy silhouette crimson Fresnel rim highlight (mode == 2)
    if (mode > 1.5) {
        float fres = pow(1.0 - saturate(dot(N, V)), 2.0);
        lit += float3(1.85, 0.14, 0.24) * fres * 1.05;
    }
    lit += emissive;

    float dist = length(i.world - eyePos.xyz);
    // Keep near-camera viewmodel free of distance fog
    float fog = mode > 0.5 && dist < 2.2 ? 0.0 : (1.0 - exp(-max(dist - 4.0, 0.0) * fogColor.w));
    lit = lerp(lit, fogColor.rgb, saturate(fog));
    return float4(encode_output(lit), 1.0);
}

// --- 3D World-Space In-Game UI Quad Shader (samples Weft mipmapped BGRA8 texture) ---
cbuffer PanelCB : register(b1) {
    float4 pCenter;
    float4 pRight;
    float4 pUp;
    float4 pTint;   // rgb tint, w alpha scale
};
Texture2D panelTex : register(t0);
SamplerState panelSamp : register(s0);
struct PanelOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float3 world : TEXCOORD1; };
PanelOut panel_vs(uint v : SV_VertexID) {
    float2 corners[6] = { float2(0,0), float2(1,0), float2(0,1), float2(0,1), float2(1,0), float2(1,1) };
    float2 q = corners[v];
    PanelOut o;
    o.uv = q;
    o.world = pCenter.xyz + pRight.xyz * (q.x * 2.0 - 1.0) + pUp.xyz * (1.0 - q.y * 2.0);
    o.pos = mul(float4(o.world, 1.0), viewProj);
    return o;
}
float4 panel_fs(PanelOut i) : SV_TARGET {
    float4 c = panelTex.Sample(panelSamp, i.uv);
    // Optional subtle vertical fade for floor reflections when pTint.w < 0.99
    float fade = pTint.w < 0.99 ? saturate(1.0 - (1.0 - i.uv.y) * 1.25) * pTint.w : 1.0;
    return float4(c.rgb * pTint.rgb * fade, c.a * fade);
}
)";

    struct alignas(16) FrameCB {
        Mat4 viewProj;
        float eyePos[4];
        float keyDir[4];
        float keyColor[4];
        float fillDir[4];
        float fillColor[4];
        float rimColor[4];
        float fogColor[4];
        float skyBottom[4];
        float skyTop[4];
        float floorGlowA[4];
        float floorColA[4];
        float floorGlowB[4];
        float floorColB[4];
    };

    struct alignas(16) PanelCB {
        float center[4];
        float right[4];
        float up[4];
        float tint[4];
    };

    ID3DBlob* compile_shader(const char* entry, const char* target) {
        const std::string src = std::string(kSceneHlsl) + kSceneHlslB;
        ID3DBlob* blob{};
        ID3DBlob* err{};
        const HRESULT hr = D3DCompile(src.data(), src.size(), "scene.hlsl", nullptr, nullptr, entry, target,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
        if (FAILED(hr)) {
            std::string msg = err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) : "compile failed";
            if (err) err->Release();
            throw Failure(std::string(entry) + ": " + msg);
        }
        if (err) err->Release();
        return blob;
    }

    // ---- Helper to load a Weft page folder into a View ----
    weft_result read_resource(void*, const char* path, std::size_t size, weft_resource_writer* file) {
        std::ifstream in(utf8_path(std::string(path, size)), std::ios::binary);
        if (!in) return WEFT_OK;
        const std::string bytes{std::istreambuf_iterator<char>(in), {}};
        return weft_resource_writer_set(file, bytes.data(), bytes.size());
    }

    struct LoadedView {
        weft_view_handle handle{};
        unsigned pixel_w{};
        unsigned pixel_h{};
    };

    LoadedView load_weft_page(weft_runtime* rt, const std::filesystem::path& dir, float logical_w, float logical_h, float scale) {
        const weft_view_desc vd{sizeof(vd), 0, logical_w, logical_h, scale, 0};
        weft_view_handle view{};
        check(weft_view_create(rt, &vd, &view), "view_create");
        weft_view_set_resource_reader(rt, view, &read_resource, nullptr);

        const std::string js = read_text(dir / "page.js");
        const std::string css = std::filesystem::exists(dir / "page.css") ? read_text(dir / "page.css") : "";
        const std::string base = utf8_str(dir / "page.css");
        const weft_script script{"page.js", 7, js.data(), js.size()};
        weft_page_desc pd{sizeof(pd)};
        pd.flags = WEFT_PAGE_WEB_PLATFORM;
        pd.scripts = &script;
        pd.script_count = 1;
        pd.stylesheet = css.data();
        pd.stylesheet_size = css.size();
        pd.base = base.data();
        pd.base_size = base.size();
        check(weft_view_load(rt, view, &pd), ("load " + dir.string()).c_str());

        unsigned diags = 0;
        for (int f = 0; f < 10; ++f) {
            check(weft_view_advance(rt, view, f / 60.0), "advance");
            weft_view_drain_diagnostics(
                rt, view,
                [](void* u, weft_view_handle, const weft_diagnostic* d) {
                    std::printf("[diag] %.*s\n", int(d->message_size), d->message);
                    ++(*static_cast<unsigned*>(u));
                },
                &diags, nullptr);
        }
        check(diags == 0, "page had diagnostics: " + dir.string());
        return {view, unsigned(std::ceil(logical_w * scale)), unsigned(std::ceil(logical_h * scale))};
    }

    // ---- Common D3D11 3D Pipeline Bundle ----
    struct Gpu3D {
        ID3D11Device* dev{};
        ID3D11DeviceContext* ctx{};
        ID3D11VertexShader* sky_vs{};
        ID3D11PixelShader* sky_ps{};
        ID3D11VertexShader* floor_vs{};
        ID3D11PixelShader* floor_ps{};
        ID3D11VertexShader* mesh_vs{};
        ID3D11PixelShader* mesh_ps{};
        ID3D11PixelShader* planet_ps{};
        ID3D11PixelShader* whitebox_ps{};
        ID3D11VertexShader* panel_vs{};
        ID3D11PixelShader* panel_ps{};
        ID3D11InputLayout* mesh_layout{};
        ID3D11Buffer* frame_cb{};
        ID3D11Buffer* panel_cb{};
        ID3D11DepthStencilState* ds_off{};
        ID3D11DepthStencilState* ds_write{};
        ID3D11DepthStencilState* ds_read{};
        ID3D11RasterizerState* rs_cull{};
        ID3D11RasterizerState* rs_none{};
        ID3D11BlendState* bs_opaque{};
        ID3D11BlendState* bs_premul{};
        ID3D11SamplerState* samp_aniso{};

        Gpu3D() {
            const D3D_FEATURE_LEVEL lvl[]{D3D_FEATURE_LEVEL_11_0};
            check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                              D3D11_CREATE_DEVICE_BGRA_SUPPORT, lvl, 1, D3D11_SDK_VERSION,
                                              &dev, nullptr, &ctx)),
                  "D3D11CreateDevice");

            const auto mk_vs = [&](const char* e, ID3D11VertexShader** out, ID3DBlob** keep = nullptr) {
                ID3DBlob* b = compile_shader(e, "vs_5_0");
                check(SUCCEEDED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, out)), e);
                if (keep) *keep = b; else b->Release();
            };
            const auto mk_ps = [&](const char* e, ID3D11PixelShader** out) {
                ID3DBlob* b = compile_shader(e, "ps_5_0");
                check(SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, out)), e);
                b->Release();
            };
            ID3DBlob* mesh_blob{};
            mk_vs("sky_vs", &sky_vs); mk_ps("sky_fs", &sky_ps);
            mk_vs("floor_vs", &floor_vs); mk_ps("floor_fs", &floor_ps);
            mk_vs("mesh_vs", &mesh_vs, &mesh_blob); mk_ps("mesh_fs", &mesh_ps);
            mk_ps("planet_fs", &planet_ps);
            mk_ps("whitebox_fs", &whitebox_ps);
            mk_vs("panel_vs", &panel_vs); mk_ps("panel_fs", &panel_ps);

            D3D11_INPUT_ELEMENT_DESC elems[]{
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
            };
            check(SUCCEEDED(dev->CreateInputLayout(elems, 4, mesh_blob->GetBufferPointer(), mesh_blob->GetBufferSize(), &mesh_layout)), "layout");
            mesh_blob->Release();

            D3D11_BUFFER_DESC cbd{};
            cbd.Usage = D3D11_USAGE_DEFAULT;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbd.ByteWidth = sizeof(FrameCB);
            check(SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &frame_cb)), "frame_cb");
            cbd.ByteWidth = sizeof(PanelCB);
            check(SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &panel_cb)), "panel_cb");

            D3D11_DEPTH_STENCIL_DESC dsd{};
            dsd.DepthEnable = FALSE;
            dev->CreateDepthStencilState(&dsd, &ds_off);
            dsd.DepthEnable = TRUE;
            dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
            dev->CreateDepthStencilState(&dsd, &ds_write);
            dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            dev->CreateDepthStencilState(&dsd, &ds_read);

            D3D11_RASTERIZER_DESC rsd{};
            rsd.FillMode = D3D11_FILL_SOLID;
            rsd.CullMode = D3D11_CULL_NONE; // clean for sculpted procedural shells
            rsd.MultisampleEnable = TRUE;
            rsd.AntialiasedLineEnable = TRUE;
            dev->CreateRasterizerState(&rsd, &rs_none);
            rsd.CullMode = D3D11_CULL_BACK;
            dev->CreateRasterizerState(&rsd, &rs_cull);

            D3D11_BLEND_DESC bsd{};
            bsd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            dev->CreateBlendState(&bsd, &bs_opaque);
            bsd.RenderTarget[0].BlendEnable = TRUE;
            bsd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            bsd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            bsd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            bsd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            bsd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            bsd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            dev->CreateBlendState(&bsd, &bs_premul);

            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_ANISOTROPIC;
            sd.MaxAnisotropy = 16;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            dev->CreateSamplerState(&sd, &samp_aniso);
        }

        ID3D11Buffer* upload_mesh(const MeshBuilder& mb) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = UINT(mb.verts.size() * sizeof(Vertex));
            bd.Usage = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA init{mb.verts.data(), 0, 0};
            ID3D11Buffer* vb{};
            check(SUCCEEDED(dev->CreateBuffer(&bd, &init, &vb)), "mesh vb");
            return vb;
        }
    };

    void readback_png(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, unsigned w, unsigned h,
                      const std::filesystem::path& out_path) {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        d.BindFlags = 0;
        d.MiscFlags = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* stg{};
        check(SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &stg)), "staging");
        ctx->CopyResource(stg, tex);
        D3D11_MAPPED_SUBRESOURCE map{};
        check(SUCCEEDED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &map)), "map");
        std::vector<std::uint8_t> pixels(std::size_t(w) * h * 4);
        for (unsigned y = 0; y < h; ++y) {
            std::memcpy(pixels.data() + std::size_t(y) * w * 4,
                        static_cast<const std::uint8_t*>(map.pData) + std::size_t(y) * map.RowPitch, w * 4);
        }
        ctx->Unmap(stg, 0);
        stg->Release();
        write_png_bgra(out_path, w, h, pixels);
        std::printf("[shot] wrote %s (%ux%u)\n", utf8_str(out_path).c_str(), w, h);
    }

    // =========================================================================
    // MODE 1: CRPG with Live D3D11 3D Render Texture (-weft-texture(stage3d))
    // =========================================================================
    int run_crpg(const std::filesystem::path& page_dir, const std::filesystem::path& out_path) {
        Gpu3D gpu;
        const unsigned W = 1920, H = 1080;
        const unsigned RT_W = 900, RT_H = 1120;

        // 1. Build Procedural Dark-Fantasy 3D Relic Sanctuary Geometry
        MeshBuilder mb;
        const Material darkStone{{0.025f, 0.022f, 0.024f}, {0, 0, 0}, 0.12f, 0.82f};
        const Material warmStone{{0.045f, 0.038f, 0.035f}, {0, 0, 0}, 0.18f, 0.70f};
        const Material gold{{0.78f, 0.54f, 0.16f}, {0.03f, 0.015f, 0.0f}, 0.94f, 0.20f};
        const Material obsidianBlade{{0.055f, 0.058f, 0.082f}, {0, 0, 0}, 0.96f, 0.14f};
        const Material emberGlow{{1.0f, 0.58f, 0.14f}, {2.8f, 1.15f, 0.18f}, 0.1f, 0.2f};
        const Material rubyGem{{0.85f, 0.06f, 0.10f}, {2.0f, 0.16f, 0.10f}, 0.2f, 0.12f};
        const Material leather{{0.048f, 0.020f, 0.014f}, {0, 0, 0}, 0.08f, 0.85f};

        // Background Gothic Cathedral Columns (receding left & right)
        for (int side : {-1, 1}) {
            for (int k = 0; k < 3; ++k) {
                const float x = side * (2.25f + k * 0.55f);
                const float z = -2.2f - k * 2.4f;
                mb.prism(8, 0.34f, 0.30f, 7.6f, darkStone, Mat4::translate(x, 3.6f, z));
                mb.prism(8, 0.44f, 0.36f, 0.45f, warmStone, Mat4::translate(x, 0.22f, z));
                mb.torus(0.35f, 0.035f, 24, 10, gold, Mat4::translate(x, 0.46f, z));
            }
        }
        // Background Gothic Pointed Arch Rib Framing Center Relic
        for (int side : {-1, 1}) {
            const Mat4 arch = Mat4::rot_z(side * 0.26f) * Mat4::translate(side * 1.45f, 3.9f, -2.5f);
            mb.box(0.24f, 3.6f, 0.32f, warmStone, arch);
        }

        // Carved Octagonal Stone Pedestal
        mb.prism(8, 1.38f, 1.24f, 0.24f, darkStone, Mat4::translate(0, 0.12f, 0));
        mb.prism(8, 1.12f, 1.02f, 0.32f, warmStone, Mat4::translate(0, 0.38f, 0));
        mb.prism(8, 1.08f, 0.96f, 0.12f, darkStone, Mat4::translate(0, 0.60f, 0));
        mb.torus(1.06f, 0.028f, 48, 12, gold, Mat4::translate(0, 0.56f, 0));
        mb.torus(1.26f, 0.022f, 48, 10, gold, Mat4::translate(0, 0.24f, 0));
        // Floating Glowing Runic Circle above Pedestal
        mb.torus(0.86f, 0.015f, 64, 10, emberGlow, Mat4::translate(0, 0.78f, 0));

        // Centerpiece Sculpted Greatsword "ECLIPSE, BLADE OF ASH"
        const Mat4 swordXf = Mat4::rot_x(0.06f) * Mat4::rot_y(0.46f) * Mat4::rot_z(-0.08f) * Mat4::translate(0.0f, 1.28f, 0.0f);
        // Main obsidian faceted blade
        mb.blade(2.22f, 0.20f, 0.065f, 0.46f, obsidianBlade, Mat4::translate(0, 0.68f, 0) * swordXf);
        // Molten golden fuller core strip running up the center of the blade
        mb.blade(1.76f, 0.044f, 0.082f, 0.26f, emberGlow, Mat4::translate(0, 0.74f, 0) * swordXf);
        // Swept Geometric Gold Crossguard
        mb.box(0.28f, 0.14f, 0.16f, gold, Mat4::translate(0, 0.66f, 0) * swordXf);
        for (int side : {-1, 1}) {
            const Mat4 wing = Mat4::rot_z(side * 0.34f) * Mat4::translate(side * 0.34f, 0.74f, 0) * swordXf;
            mb.prism(4, 0.07f, 0.025f, 0.54f, gold, Mat4::rot_z(-side * kPi * 0.5f) * wing);
            mb.octahedron(0.06f, 0.06f, 0.06f, emberGlow, Mat4::translate(side * 0.56f, 0.83f, 0) * swordXf);
        }
        // Central Glowing Ruby Heart Gem embedded in the Crossguard
        mb.octahedron(0.085f, 0.125f, 0.105f, rubyGem, Mat4::translate(0, 0.66f, 0.02f) * swordXf);
        // Octagonal Grip & Golden Spiked Pommel
        mb.prism(8, 0.052f, 0.058f, 0.56f, leather, Mat4::translate(0, 0.34f, 0) * swordXf);
        mb.torus(0.062f, 0.014f, 20, 8, gold, Mat4::translate(0, 0.52f, 0) * swordXf);
        mb.torus(0.060f, 0.014f, 20, 8, gold, Mat4::translate(0, 0.18f, 0) * swordXf);
        mb.octahedron(0.11f, 0.14f, 0.11f, gold, Mat4::translate(0, 0.02f, 0) * swordXf);

        // 3 Counter-Tilted Concentric Golden Armillary Rings orbiting the Relic
        const Mat4 ring1 = Mat4::rot_z(0.36f) * Mat4::rot_x(0.52f) * Mat4::translate(0, 2.22f, 0);
        const Mat4 ring2 = Mat4::rot_z(-0.58f) * Mat4::rot_x(1.08f) * Mat4::translate(0, 2.22f, 0);
        const Mat4 ring3 = Mat4::rot_z(0.18f) * Mat4::rot_y(0.45f) * Mat4::rot_x(1.36f) * Mat4::translate(0, 2.22f, 0);
        mb.torus(1.08f, 0.018f, 72, 12, gold, ring1);
        mb.torus(1.34f, 0.015f, 72, 12, gold, ring2);
        mb.torus(1.56f, 0.012f, 72, 10, emberGlow, ring3);

        // Orbiting Runic Nodes on the Rings + Floating Ember Shards
        for (int k = 0; k < 12; ++k) {
            const float a = k * (2.f * kPi / 12.f) + 0.3f;
            const float r = 0.85f + (k % 3) * 0.38f;
            const float y = 0.95f + (k % 5) * 0.62f;
            const Mat4 xf = Mat4::rot_y(a * 1.7f) * Mat4::rot_x(a * 0.9f) *
                            Mat4::translate(std::cos(a) * r, y, std::sin(a) * r * 0.7f);
            const bool hot = (k % 2 == 0);
            mb.octahedron(hot ? 0.036f : 0.055f, hot ? 0.068f : 0.095f, hot ? 0.036f : 0.055f,
                          hot ? emberGlow : obsidianBlade, xf);
        }

        // 2. Render 3D Scene into 4x MSAA RGBA16F Target -> Resolve to Linear SRV Texture (stage3d)
        D3D11_TEXTURE2D_DESC rt_ms_desc{};
        rt_ms_desc.Width = RT_W; rt_ms_desc.Height = RT_H;
        rt_ms_desc.MipLevels = rt_ms_desc.ArraySize = 1;
        rt_ms_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rt_ms_desc.SampleDesc.Count = 4;
        rt_ms_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D* rt_ms{};
        ID3D11RenderTargetView* rt_ms_rtv{};
        check(SUCCEEDED(gpu.dev->CreateTexture2D(&rt_ms_desc, nullptr, &rt_ms)) &&
                  SUCCEEDED(gpu.dev->CreateRenderTargetView(rt_ms, nullptr, &rt_ms_rtv)),
              "crpg msaa rt");

        D3D11_TEXTURE2D_DESC ds_desc = rt_ms_desc;
        ds_desc.Format = DXGI_FORMAT_D32_FLOAT;
        ds_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ID3D11Texture2D* ds_tex{};
        ID3D11DepthStencilView* dsv{};
        check(SUCCEEDED(gpu.dev->CreateTexture2D(&ds_desc, nullptr, &ds_tex)) &&
                  SUCCEEDED(gpu.dev->CreateDepthStencilView(ds_tex, nullptr, &dsv)),
              "crpg dsv");

        D3D11_TEXTURE2D_DESC stage_desc{};
        stage_desc.Width = RT_W; stage_desc.Height = RT_H;
        stage_desc.MipLevels = stage_desc.ArraySize = 1;
        stage_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        stage_desc.SampleDesc.Count = 1;
        stage_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* stage_tex{};
        ID3D11ShaderResourceView* stage_srv{};
        check(SUCCEEDED(gpu.dev->CreateTexture2D(&stage_desc, nullptr, &stage_tex)) &&
                  SUCCEEDED(gpu.dev->CreateShaderResourceView(stage_tex, nullptr, &stage_srv)),
              "crpg stage srv");

        ID3D11Buffer* vb = gpu.upload_mesh(mb);

        const Vec3 eye{0.0f, 2.38f, 6.25f};
        const Vec3 target{0.0f, 2.15f, 0.0f};
        FrameCB fcb{};
        fcb.viewProj = Mat4::look_at_rh(eye, target, {0, 1, 0}) * Mat4::perspective_rh(0.76f, float(RT_W) / float(RT_H), 0.1f, 80.f);
        fcb.eyePos[0] = eye.x; fcb.eyePos[1] = eye.y; fcb.eyePos[2] = eye.z; fcb.eyePos[3] = 1.f;
        // Dramatic chiaroscuro: warm amber torchlight left + cool moonlit gothic rim right
        fcb.keyDir[0] = -0.68f; fcb.keyDir[1] = 0.42f; fcb.keyDir[2] = 0.62f; fcb.keyDir[3] = 2.8f;
        fcb.keyColor[0] = 1.0f; fcb.keyColor[1] = 0.66f; fcb.keyColor[2] = 0.26f;
        fcb.fillDir[0] = 0.78f; fcb.fillDir[1] = 0.72f; fcb.fillDir[2] = -0.25f; fcb.fillDir[3] = 1.35f;
        fcb.fillColor[0] = 0.28f; fcb.fillColor[1] = 0.44f; fcb.fillColor[2] = 0.88f;
        fcb.rimColor[0] = 1.0f; fcb.rimColor[1] = 0.58f; fcb.rimColor[2] = 0.18f; fcb.rimColor[3] = 2.8f;
        fcb.fogColor[0] = 0.0025f; fcb.fogColor[1] = 0.0020f; fcb.fogColor[2] = 0.0032f; fcb.fogColor[3] = 0.16f;
        fcb.skyBottom[0] = 0.0025f; fcb.skyBottom[1] = 0.0020f; fcb.skyBottom[2] = 0.0024f; fcb.skyBottom[3] = 0.0f;
        fcb.skyTop[0] = 0.0022f; fcb.skyTop[1] = 0.0032f; fcb.skyTop[2] = 0.0075f;
        fcb.floorGlowA[0] = 0.0f; fcb.floorGlowA[1] = 0.0f; fcb.floorGlowA[2] = 0.0f; fcb.floorGlowA[3] = 0.9f;
        fcb.floorColA[0] = 1.0f; fcb.floorColA[1] = 0.48f; fcb.floorColA[2] = 0.10f; fcb.floorColA[3] = 0.16f;
        fcb.floorGlowB[3] = 1.f;

        gpu.ctx->UpdateSubresource(gpu.frame_cb, 0, nullptr, &fcb, 0, 0);
        const float clr[4]{0, 0, 0, 1};
        gpu.ctx->ClearRenderTargetView(rt_ms_rtv, clr);
        gpu.ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT vp{0, 0, float(RT_W), float(RT_H), 0, 1};
        gpu.ctx->RSSetViewports(1, &vp);
        gpu.ctx->OMSetRenderTargets(1, &rt_ms_rtv, dsv);
        gpu.ctx->OMSetBlendState(gpu.bs_opaque, nullptr, 0xffffffff);
        gpu.ctx->RSSetState(gpu.rs_none);
        gpu.ctx->VSSetConstantBuffers(0, 1, &gpu.frame_cb);
        gpu.ctx->PSSetConstantBuffers(0, 1, &gpu.frame_cb);
        gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Draw Sky
        gpu.ctx->OMSetDepthStencilState(gpu.ds_off, 0);
        gpu.ctx->IASetInputLayout(nullptr);
        gpu.ctx->VSSetShader(gpu.sky_vs, nullptr, 0);
        gpu.ctx->PSSetShader(gpu.sky_ps, nullptr, 0);
        gpu.ctx->Draw(3, 0);

        // Draw Floor
        gpu.ctx->OMSetDepthStencilState(gpu.ds_write, 0);
        gpu.ctx->VSSetShader(gpu.floor_vs, nullptr, 0);
        gpu.ctx->PSSetShader(gpu.floor_ps, nullptr, 0);
        gpu.ctx->Draw(6, 0);

        // Draw 3D Relic Sanctuary Geometry
        UINT stride = sizeof(Vertex), offset = 0;
        gpu.ctx->IASetInputLayout(gpu.mesh_layout);
        gpu.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        gpu.ctx->VSSetShader(gpu.mesh_vs, nullptr, 0);
        gpu.ctx->PSSetShader(gpu.mesh_ps, nullptr, 0);
        gpu.ctx->Draw(UINT(mb.verts.size()), 0);

        // Resolve 4x MSAA -> stage_tex (zero-copy GPU SRV for Weft!)
        gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        gpu.ctx->ResolveSubresource(stage_tex, 0, rt_ms, 0, DXGI_FORMAT_R16G16B16A16_FLOAT);

        // 3. Boot Weft, Import stage3d External Texture Zero-Copy, and Render CRPG UI!
        weft_runtime* rt{};
        const weft_runtime_desc rtd{sizeof(rtd), WEFT_ABI_VERSION, 8, WEFT_RUNTIME_SYSTEM_FONTS};
        check(weft_runtime_create(&rtd, &rt), "runtime");
        const auto view = load_weft_page(rt, page_dir, 1280.f, 720.f, 1.5f);

        constexpr std::uint64_t queue = 1;
        const weft_render_d3d11_desc bd{sizeof(bd), WEFT_RENDER_ABI_VERSION, gpu.dev, queue, 0};
        weft_render_d3d11* backend{};
        weft_render_backend table{};
        check(weft_render_d3d11_create(&bd, &backend, &table), "d3d11_create");
        weft_renderer* renderer{};
        check(weft_renderer_create(&table, nullptr, &renderer), "renderer_create");
        weft_render_external_backend ext{};
        check(weft_render_d3d11_external(backend, &ext), "d3d11_external");
        check(weft_renderer_install_external_backend(renderer, &ext), "install_external");

        weft_render_info info{};
        info.struct_size = sizeof(info);
        check(weft_renderer_get_info(renderer, &info), "get_info");

        const weft_render_d3d11_texture native_tex{sizeof(native_tex), WEFT_RENDER_EXTERNAL_ABI_VERSION, stage_srv, 0};
        const weft_render_external_texture ext_desc{
            sizeof(ext_desc), WEFT_RENDER_EXTERNAL_ABI_VERSION, info.identity,
            101, 1, RT_W, RT_H,
            WEFT_RENDER_TARGET_LINEAR_RGBA16F,
            WEFT_RENDER_EXTERNAL_LINEAR_PREMULTIPLIED,
            WEFT_RENDER_EXTERNAL_TOP_LEFT,
            WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE,
            1, 0, &native_tex, sizeof(native_tex), nullptr, nullptr};
        weft_render_handle stage_lease{};
        check(weft_renderer_import_texture(renderer, &ext_desc, &stage_lease), "import stage3d");

        // Final 1920x1080 Backbuffer
        D3D11_TEXTURE2D_DESC back_desc{};
        back_desc.Width = W; back_desc.Height = H;
        back_desc.MipLevels = back_desc.ArraySize = 1;
        back_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        back_desc.SampleDesc.Count = 1;
        back_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* back{};
        ID3D11RenderTargetView* back_rtv{};
        check(SUCCEEDED(gpu.dev->CreateTexture2D(&back_desc, nullptr, &back)) &&
                  SUCCEEDED(gpu.dev->CreateRenderTargetView(back, nullptr, &back_rtv)),
              "backbuffer");
        const float bg[4]{0.03f, 0.027f, 0.027f, 1.f};
        gpu.ctx->ClearRenderTargetView(back_rtv, bg);

        weft_view_frame* frame{};
        check(weft_view_acquire_frame(rt, view.handle, &frame), "acquire_frame");
        const weft_render_scene_texture bound{"stage3d", 7, 0, stage_lease};
        const weft_render_scene_frame sf{sizeof(sf), WEFT_RENDER_ABI_VERSION, frame, 0, 1, &bound};
        weft_render_handle prepared{};
        check(weft_renderer_prepare_scene(renderer, &sf, &prepared), "prepare_scene");
        weft_view_frame_release(frame);

        weft_render_d3d11_pass payload{sizeof(payload), WEFT_RENDER_ABI_VERSION, nullptr, back_rtv, nullptr, 0, 0, {}};
        const weft_render_pass pass{sizeof(pass), WEFT_RENDER_ABI_VERSION, info.identity, W, H,
                                    WEFT_RENDER_TARGET_LINEAR_BGRA8, 1, &payload, sizeof(payload)};
        weft_render_handle use{};
        const weft_render_result enc = weft_renderer_encode(renderer, prepared, &pass, &use);
        if (enc != WEFT_RENDER_OK) {
            weft_renderer_last_failure(renderer, [](void*, const char* t, size_t n) {
                std::fprintf(stderr, "encode failure: %.*s\n", int(n), t);
            }, nullptr);
            check(enc, "encode crpg");
        }
        weft_renderer_release_prepared(renderer, prepared);
        gpu.ctx->Flush();
        weft_renderer_notify_submitted(renderer, use, queue, 1);
        weft_renderer_notify_completed(renderer, info.identity.device_epoch, queue, 1);

        readback_png(gpu.dev, gpu.ctx, back, W, H, out_path);

        weft_renderer_release_texture(renderer, stage_lease);
        weft_renderer_destroy(renderer);
        weft_render_d3d11_destroy(backend);
        weft_view_destroy(rt, view.handle);
        weft_runtime_destroy(rt);
        return 0;
    }

    // =========================================================================
    // MODE 2: JRPG with True 3D In-Game Diegetic UI Panels in 3D Geometric Arena
    // =========================================================================
    struct RenderedPanel {
        ID3D11Texture2D* tex{};
        ID3D11RenderTargetView* rtv{};
        ID3D11ShaderResourceView* srv{};
        unsigned w{}, h{};
    };

    RenderedPanel render_weft_panel_texture(Gpu3D& gpu, weft_runtime* rt, weft_renderer* renderer,
                                            const weft_render_info& info, const std::filesystem::path& dir,
                                            float logical_w, float logical_h, float scale, std::uint64_t& serial) {
        const auto view = load_weft_page(rt, dir, logical_w, logical_h, scale);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = view.pixel_w;
        td.Height = view.pixel_h;
        td.MipLevels = 0; // full mipmap chain for crisp 3D perspective sampling!
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        RenderedPanel out{};
        out.w = view.pixel_w;
        out.h = view.pixel_h;
        check(SUCCEEDED(gpu.dev->CreateTexture2D(&td, nullptr, &out.tex)), "panel tex");
        D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
        rtvd.Format = td.Format;
        rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtvd.Texture2D.MipSlice = 0;
        check(SUCCEEDED(gpu.dev->CreateRenderTargetView(out.tex, &rtvd, &out.rtv)), "panel rtv");
        check(SUCCEEDED(gpu.dev->CreateShaderResourceView(out.tex, nullptr, &out.srv)), "panel srv");

        weft_view_frame* frame{};
        check(weft_view_acquire_frame(rt, view.handle, &frame), "panel acquire");
        const weft_render_scene_frame sf{sizeof(sf), WEFT_RENDER_ABI_VERSION, frame, 0, 0, nullptr};
        weft_render_handle prepared{};
        check(weft_renderer_prepare_scene(renderer, &sf, &prepared), "panel prepare");
        weft_view_frame_release(frame);

        weft_render_d3d11_pass payload{sizeof(payload), WEFT_RENDER_ABI_VERSION, nullptr, out.rtv, nullptr, 1, 0, {0, 0, 0, 0}};
        const weft_render_pass pass{sizeof(pass), WEFT_RENDER_ABI_VERSION, info.identity, out.w, out.h,
                                    WEFT_RENDER_TARGET_LINEAR_BGRA8, 1, &payload, sizeof(payload)};
        weft_render_handle use{};
        const weft_render_result enc = weft_renderer_encode(renderer, prepared, &pass, &use);
        if (enc != WEFT_RENDER_OK) {
            weft_renderer_last_failure(renderer, [](void*, const char* t, size_t n) {
                std::fprintf(stderr, "panel encode fail: %.*s\n", int(n), t);
            }, nullptr);
            check(enc, ("encode panel " + dir.string()).c_str());
        }
        weft_renderer_release_prepared(renderer, prepared);
        gpu.ctx->GenerateMips(out.srv);
        ++serial;
        weft_renderer_notify_submitted(renderer, use, 1, serial);
        weft_renderer_notify_completed(renderer, info.identity.device_epoch, 1, serial);
        weft_view_destroy(rt, view.handle);
        return out;
    }

    int run_jrpg(const std::filesystem::path& page_dir, const std::filesystem::path& out_path) {
        Gpu3D gpu;
        const unsigned W = 1920, H = 1080;

        // 1. Render 4 Weft UI Panels into Mipmapped D3D11 Textures + Prepare Screen HUD View
        weft_runtime* rt{};
        const weft_runtime_desc rtd{sizeof(rtd), WEFT_ABI_VERSION, 12, WEFT_RUNTIME_SYSTEM_FONTS};
        check(weft_runtime_create(&rtd, &rt), "runtime");

        constexpr std::uint64_t queue = 1;
        const weft_render_d3d11_desc bd{sizeof(bd), WEFT_RENDER_ABI_VERSION, gpu.dev, queue, 0};
        weft_render_d3d11* backend{};
        weft_render_backend table{};
        check(weft_render_d3d11_create(&bd, &backend, &table), "d3d11_create");
        weft_renderer* renderer{};
        check(weft_renderer_create(&table, nullptr, &renderer), "renderer_create");
        weft_render_info info{};
        info.struct_size = sizeof(info);
        check(weft_renderer_get_info(renderer, &info), "get_info");

        std::uint64_t serial = 0;
        auto p_cmd = render_weft_panel_texture(gpu, rt, renderer, info, page_dir / "panel-cmd", 510.f, 410.f, 2.0f, serial);
        auto p_boss = render_weft_panel_texture(gpu, rt, renderer, info, page_dir / "panel-boss", 450.f, 175.f, 2.0f, serial);
        auto p_impact = render_weft_panel_texture(gpu, rt, renderer, info, page_dir / "panel-impact", 320.f, 240.f, 2.0f, serial);
        auto p_party = render_weft_panel_texture(gpu, rt, renderer, info, page_dir / "panel-party", 720.f, 165.f, 2.0f, serial);
        auto hud_view = load_weft_page(rt, page_dir / "hud-overlay", 1280.f, 720.f, 1.5f);

        // 2. Build Stylized 3D Geometric Anime Battle Arena
        MeshBuilder mb;
        const Material monolithMat{{0.025f, 0.024f, 0.055f}, {0.0f, 0.0f, 0.01f}, 0.88f, 0.24f};
        const Material armorMat{{0.045f, 0.042f, 0.078f}, {0.01f, 0.0f, 0.02f}, 0.92f, 0.18f};
        const Material bossGold{{0.95f, 0.72f, 0.18f}, {0.32f, 0.18f, 0.02f}, 0.94f, 0.18f};
        const Material crimsonCloak{{0.42f, 0.012f, 0.055f}, {0.16f, 0.006f, 0.025f}, 0.22f, 0.50f};
        const Material swordCyan{{0.55f, 0.92f, 1.0f}, {0.95f, 1.85f, 2.6f}, 0.4f, 0.15f};
        const Material heroCoat{{0.032f, 0.038f, 0.068f}, {0, 0, 0}, 0.68f, 0.35f};
        const Material heroScarf{{0.82f, 0.02f, 0.09f}, {0.36f, 0.01f, 0.04f}, 0.15f, 0.55f};
        const Material laserRed{{1.0f, 0.15f, 0.28f}, {2.6f, 0.22f, 0.42f}, 0.1f, 0.2f};
        const Material yellowRing{{1.0f, 0.88f, 0.12f}, {1.6f, 1.2f, 0.08f}, 0.2f, 0.2f};

        // Floating Obsidian Monolith Pillars & Crystal Shards framing the 3D arena
        struct Pillar { float x, y, z, sx, sy, sz, ry, rz; };
        const Pillar pillars[]{
            {-6.2f, 2.8f, -5.8f, 0.9f, 4.4f, 0.9f, 0.4f,  0.12f},
            {-4.6f, 4.2f, -7.8f, 0.6f, 2.8f, 0.6f, 0.9f, -0.15f},
            { 6.4f, 3.2f, -5.2f, 1.0f, 4.8f, 1.0f,-0.5f, -0.14f},
            { 5.0f, 4.6f, -8.2f, 0.7f, 3.2f, 0.7f, 0.3f,  0.10f},
            {-7.6f, 1.8f, -1.8f, 0.8f, 3.4f, 0.8f, 0.2f,  0.08f},
            { 7.2f, 2.0f, -1.2f, 0.8f, 3.6f, 0.8f,-0.4f, -0.09f},
        };
        for (const auto& p : pillars) {
            const Mat4 xf = Mat4::rot_z(p.rz) * Mat4::rot_y(p.ry) * Mat4::translate(p.x, p.y, p.z);
            mb.box(p.sx, p.sy, p.sz, monolithMat, xf);
            // Glowing crimson accent trim band around each monolith
            mb.box(p.sx * 1.04f, 0.08f, p.sz * 1.04f, laserRed, Mat4::translate(0, p.sy * 0.22f, 0) * xf);
        }

        // Dramatic Geometric Boss: "PHANTOM ARCHON" at (0.85, 0.0, -3.1)
        const Vec3 bossPos{0.85f, 2.15f, -3.1f};
        const Mat4 bossXf = Mat4::rot_y(-0.22f) * Mat4::translate(bossPos.x, bossPos.y, bossPos.z);
        // Sculpted dark armored diamond torso & floating lower greaves
        mb.octahedron(0.62f, 0.92f, 0.44f, armorMat, bossXf);
        mb.prism(6, 0.28f, 0.46f, 0.95f, armorMat, Mat4::translate(0, -0.95f, 0) * bossXf);
        // Spiked shoulder pauldrons
        for (int side : {-1, 1}) {
            mb.prism(4, 0.28f, 0.04f, 0.78f, armorMat,
                     Mat4::rot_z(-side * 1.02f) * Mat4::translate(side * 0.78f, 0.52f, 0.05f) * bossXf);
            // Sweeping jagged crimson-obsidian cape wings
            mb.quad({side * 0.35f, 0.55f, -0.22f}, {side * 1.85f, -0.85f, -0.68f},
                    {side * 1.32f, -1.52f, -0.46f}, {side * 0.22f, -0.45f, -0.24f}, crimsonCloak, bossXf);
            mb.quad({side * 0.38f, 0.35f, -0.32f}, {side * 2.15f, -0.18f, -0.95f},
                    {side * 1.72f, -0.78f, -0.78f}, {side * 0.30f, -0.20f, -0.30f}, armorMat, bossXf);
        }
        // Gleaming Golden Geometric Crown/Mask
        mb.octahedron(0.28f, 0.42f, 0.26f, bossGold, Mat4::translate(0, 1.08f, 0.08f) * bossXf);
        mb.torus(0.46f, 0.025f, 6, 8, bossGold, Mat4::rot_x(kPi * 0.5f) * Mat4::translate(0, 1.12f, -0.05f) * bossXf);
        // 8 Orbiting Radiant 3D Crystal Blades in a Halo behind the Boss!
        for (int k = 0; k < 8; ++k) {
            const float ang = -1.28f + k * (2.56f / 7.f);
            const float r = 1.48f;
            const float bx = std::sin(ang) * r;
            const float by = std::cos(ang) * r + 0.58f;
            const Mat4 bxf = Mat4::rot_z(-ang) * Mat4::translate(bx, by, -0.42f) * bossXf;
            mb.blade(1.18f, 0.075f, 0.03f, 0.28f, swordCyan, bxf);
            mb.box(0.24f, 0.045f, 0.05f, bossGold, Mat4::translate(0, 0.04f, 0) * bxf);
        }
        // Target Lock 3D Ring on the floor beneath the Boss
        mb.torus(1.42f, 0.026f, 64, 10, laserRed, Mat4::translate(bossPos.x, 0.03f, bossPos.z));
        mb.torus(1.05f, 0.014f, 48, 8, yellowRing, Mat4::translate(bossPos.x, 0.04f, bossPos.z));

        // Stylized 3D Geometric Hero ("REN") in foreground-left facing the Boss
        const Vec3 heroPos{-0.82f, 0.0f, 1.95f};
        const Mat4 heroXf = Mat4::rot_y(0.48f) * Mat4::translate(heroPos.x, heroPos.y, heroPos.z);
        // Hero glowing tactical stance ring on floor
        mb.torus(0.84f, 0.022f, 48, 10, swordCyan, Mat4::translate(heroPos.x, 0.03f, heroPos.z));
        // Sculpted geometric longcoat body & head prism
        mb.prism(6, 0.40f, 0.19f, 1.08f, heroCoat, Mat4::translate(0, 0.58f, 0) * heroXf);
        mb.prism(6, 0.21f, 0.28f, 0.60f, heroCoat, Mat4::translate(0, 1.32f, 0) * heroXf);
        mb.octahedron(0.19f, 0.23f, 0.19f, heroCoat, Mat4::translate(0, 1.80f, 0) * heroXf);
        // Dramatic Fluttering 3D Crimson Scarf Ribbons streaming left!
        mb.quad({-0.05f, 1.58f, 0.02f}, {-1.08f, 1.66f, 0.18f}, {-0.96f, 1.42f, 0.14f}, {-0.05f, 1.40f, 0.02f}, heroScarf, heroXf);
        mb.quad({-0.08f, 1.48f, -0.04f}, {-0.88f, 1.20f, 0.30f}, {-0.76f, 1.04f, 0.24f}, {-0.08f, 1.34f, -0.04f}, heroScarf, heroXf);
        // Blazing 3D Cyan Energy Katana held diagonally forward!
        const Mat4 katanaXf = Mat4::rot_z(-1.18f) * Mat4::rot_y(0.25f) * Mat4::translate(0.30f, 1.00f, -0.24f) * heroXf;
        mb.blade(1.36f, 0.040f, 0.022f, 0.22f, swordCyan, katanaXf);

        // 3D Glowing Crimson Tether linking Boss Crown to Floating 3D Boss UI Panel bottom-left corner!
        const Vec3 tA{bossPos.x + 0.44f, bossPos.y + 1.15f, bossPos.z + 0.06f};
        const Vec3 tB{1.95f, 3.62f, -2.55f};
        const Vec3 tMid = (tA + tB) * 0.5f;
        const Vec3 tDir = tB - tA;
        const float tLen = std::sqrt(dot(tDir, tDir));
        const float tYaw = std::atan2(tDir.x, tDir.z);
        const float tPitch = std::atan2(std::sqrt(tDir.x * tDir.x + tDir.z * tDir.z), tDir.y);
        mb.prism(6, 0.012f, 0.012f, tLen, laserRed,
                 Mat4::rot_x(tPitch) * Mat4::rot_y(tYaw) * Mat4::translate(tMid.x, tMid.y, tMid.z));

        // 3. Render 3D Scene (4x MSAA) + 3D World-Space Mipmapped Weft UI Panels + Planar Reflections!
        D3D11_TEXTURE2D_DESC ms_desc{};
        ms_desc.Width = W; ms_desc.Height = H;
        ms_desc.MipLevels = ms_desc.ArraySize = 1;
        ms_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        ms_desc.SampleDesc.Count = 4;
        ms_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D* ms_col{};
        ID3D11RenderTargetView* ms_rtv{};
        check(SUCCEEDED(gpu.dev->CreateTexture2D(&ms_desc, nullptr, &ms_col)) &&
                  SUCCEEDED(gpu.dev->CreateRenderTargetView(ms_col, nullptr, &ms_rtv)),
              "jrpg msaa");

        D3D11_TEXTURE2D_DESC ds_desc = ms_desc;
        ds_desc.Format = DXGI_FORMAT_D32_FLOAT;
        ds_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ID3D11Texture2D* ds_tex{};
        ID3D11DepthStencilView* dsv{};
        check(SUCCEEDED(gpu.dev->CreateTexture2D(&ds_desc, nullptr, &ds_tex)) &&
                  SUCCEEDED(gpu.dev->CreateDepthStencilView(ds_tex, nullptr, &dsv)),
              "jrpg dsv");

        ID3D11Buffer* vb = gpu.upload_mesh(mb);

        const Vec3 eye{-0.22f, 2.52f, 6.85f};
        const Vec3 target{0.25f, 1.82f, -1.6f};
        FrameCB fcb{};
        fcb.viewProj = Mat4::look_at_rh(eye, target, {0, 1, 0}) * Mat4::perspective_rh(0.82f, float(W) / float(H), 0.1f, 120.f);
        fcb.eyePos[0] = eye.x; fcb.eyePos[1] = eye.y; fcb.eyePos[2] = eye.z; fcb.eyePos[3] = 1.f;
        fcb.keyDir[0] = -0.45f; fcb.keyDir[1] = 0.78f; fcb.keyDir[2] = 0.45f; fcb.keyDir[3] = 2.2f;
        fcb.keyColor[0] = 0.85f; fcb.keyColor[1] = 0.88f; fcb.keyColor[2] = 1.0f;
        fcb.fillDir[0] = 0.8f; fcb.fillDir[1] = 0.25f; fcb.fillDir[2] = -0.55f; fcb.fillDir[3] = 1.9f;
        fcb.fillColor[0] = 1.0f; fcb.fillColor[1] = 0.14f; fcb.fillColor[2] = 0.45f;
        fcb.rimColor[0] = 0.35f; fcb.rimColor[1] = 0.78f; fcb.rimColor[2] = 1.0f; fcb.rimColor[3] = 2.2f;
        fcb.fogColor[0] = 0.045f; fcb.fogColor[1] = 0.018f; fcb.fogColor[2] = 0.105f; fcb.fogColor[3] = 0.042f;
        fcb.skyBottom[0] = 0.14f; fcb.skyBottom[1] = 0.02f; fcb.skyBottom[2] = 0.22f; fcb.skyBottom[3] = 1.0f; // srgb mode
        fcb.skyTop[0] = 0.012f; fcb.skyTop[1] = 0.015f; fcb.skyTop[2] = 0.085f;
        // Cyan Hero floor pool + Crimson Boss floor pool
        fcb.floorGlowA[0] = heroPos.x; fcb.floorGlowA[1] = 0; fcb.floorGlowA[2] = heroPos.z; fcb.floorGlowA[3] = 0.85f;
        fcb.floorColA[0] = 0.08f; fcb.floorColA[1] = 0.78f; fcb.floorColA[2] = 1.0f; fcb.floorColA[3] = 0.48f;
        fcb.floorGlowB[0] = bossPos.x; fcb.floorGlowB[1] = 0; fcb.floorGlowB[2] = bossPos.z; fcb.floorGlowB[3] = 0.42f;
        fcb.floorColB[0] = 1.0f; fcb.floorColB[1] = 0.08f; fcb.floorColB[2] = 0.32f; fcb.floorColB[3] = 0.65f;

        gpu.ctx->UpdateSubresource(gpu.frame_cb, 0, nullptr, &fcb, 0, 0);
        const float clr[4]{0, 0, 0, 1};
        gpu.ctx->ClearRenderTargetView(ms_rtv, clr);
        gpu.ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
        gpu.ctx->RSSetViewports(1, &vp);
        gpu.ctx->OMSetRenderTargets(1, &ms_rtv, dsv);
        gpu.ctx->OMSetBlendState(gpu.bs_opaque, nullptr, 0xffffffff);
        gpu.ctx->RSSetState(gpu.rs_none);
        gpu.ctx->VSSetConstantBuffers(0, 1, &gpu.frame_cb);
        gpu.ctx->PSSetConstantBuffers(0, 1, &gpu.frame_cb);
        gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Sky -> Floor -> 3D Arena Meshes
        gpu.ctx->OMSetDepthStencilState(gpu.ds_off, 0);
        gpu.ctx->VSSetShader(gpu.sky_vs, nullptr, 0);
        gpu.ctx->PSSetShader(gpu.sky_ps, nullptr, 0);
        gpu.ctx->Draw(3, 0);

        gpu.ctx->OMSetDepthStencilState(gpu.ds_write, 0);
        gpu.ctx->VSSetShader(gpu.floor_vs, nullptr, 0);
        gpu.ctx->PSSetShader(gpu.floor_ps, nullptr, 0);
        gpu.ctx->Draw(6, 0);

        UINT stride = sizeof(Vertex), offset = 0;
        gpu.ctx->IASetInputLayout(gpu.mesh_layout);
        gpu.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        gpu.ctx->VSSetShader(gpu.mesh_vs, nullptr, 0);
        gpu.ctx->PSSetShader(gpu.mesh_ps, nullptr, 0);
        gpu.ctx->Draw(UINT(mb.verts.size()), 0);

        // Draw 3D World-Space In-Game Weft UI Quads!
        const auto draw_3d_panel = [&](const RenderedPanel& p, Vec3 center, float half_w, float half_h,
                                       float yaw, float pitch, float roll, bool reflect, float refl_alpha = 0.24f) {
            const Mat4 rot = Mat4::rot_z(roll) * Mat4::rot_x(pitch) * Mat4::rot_y(yaw);
            const Vec3 right = xform_dir({1, 0, 0}, rot) * half_w;
            const Vec3 up = xform_dir({0, 1, 0}, rot) * half_h;

            gpu.ctx->IASetInputLayout(nullptr);
            gpu.ctx->VSSetShader(gpu.panel_vs, nullptr, 0);
            gpu.ctx->PSSetShader(gpu.panel_ps, nullptr, 0);
            gpu.ctx->VSSetConstantBuffers(1, 1, &gpu.panel_cb);
            gpu.ctx->PSSetConstantBuffers(1, 1, &gpu.panel_cb);
            gpu.ctx->PSSetShaderResources(0, 1, &p.srv);
            gpu.ctx->PSSetSamplers(0, 1, &gpu.samp_aniso);
            gpu.ctx->OMSetBlendState(gpu.bs_premul, nullptr, 0xffffffff);
            gpu.ctx->OMSetDepthStencilState(gpu.ds_read, 0);

            if (reflect) {
                // Mirrored reflection below y=0 (drawn slightly above floor plane or depth-off for glossy reflection look)
                PanelCB rcb{};
                rcb.center[0] = center.x; rcb.center[1] = -center.y; rcb.center[2] = center.z;
                rcb.right[0] = right.x;   rcb.right[1] = -right.y;   rcb.right[2] = right.z;
                rcb.up[0] = up.x;         rcb.up[1] = -up.y;         rcb.up[2] = up.z;
                rcb.tint[0] = 0.85f; rcb.tint[1] = 0.88f; rcb.tint[2] = 1.0f; rcb.tint[3] = refl_alpha;
                gpu.ctx->OMSetDepthStencilState(gpu.ds_off, 0);
                gpu.ctx->UpdateSubresource(gpu.panel_cb, 0, nullptr, &rcb, 0, 0);
                gpu.ctx->Draw(6, 0);
                gpu.ctx->OMSetDepthStencilState(gpu.ds_read, 0);
            }

            PanelCB pcb{};
            pcb.center[0] = center.x; pcb.center[1] = center.y; pcb.center[2] = center.z;
            pcb.right[0] = right.x;   pcb.right[1] = right.y;   pcb.right[2] = right.z;
            pcb.up[0] = up.x;         pcb.up[1] = up.y;         pcb.up[2] = up.z;
            pcb.tint[0] = 1.f; pcb.tint[1] = 1.f; pcb.tint[2] = 1.f; pcb.tint[3] = 1.f;
            gpu.ctx->UpdateSubresource(gpu.panel_cb, 0, nullptr, &pcb, 0, 0);
            gpu.ctx->Draw(6, 0);
        };

        // Back-to-front world-space 3D Weft UI panels!
        // 1. Boss Weakness & HP Hologram (high upper-right near Boss, angled cleanly toward viewer in 3D!)
        draw_3d_panel(p_boss, {3.45f, 4.12f, -2.25f}, 1.84f, 0.72f, -0.34f, 0.06f, -0.025f, false);
        // 2. Floating 3D WEAK!! / 2,480 CRITICAL Impact Starburst (in front of Boss right flank!)
        draw_3d_panel(p_impact, {3.15f, 2.38f, -1.45f}, 1.04f, 0.78f, -0.22f, 0.02f, 0.04f, false);
        // 3. Diegetic Command & Skill Board (floating beside Hero on left in strong 3D perspective + floor reflection!)
        draw_3d_panel(p_cmd, {-2.45f, 2.04f, 0.65f}, 1.48f, 1.19f, 0.36f, -0.04f, 0.02f, true, 0.20f);
        // 4. Angled Foreground Party Status Holo-Deck (floating right-bottom above checkerboard floor + floor reflection!)
        draw_3d_panel(p_party, {1.22f, 0.78f, 2.45f}, 1.62f, 0.38f, -0.24f, 0.16f, -0.015f, true, 0.26f);

        // 4. Resolve 4x MSAA 3D World -> Backbuffer, then Composite Crisp Screen-Space HUD View on top!
        D3D11_TEXTURE2D_DESC back_desc{};
        back_desc.Width = W; back_desc.Height = H;
        back_desc.MipLevels = back_desc.ArraySize = 1;
        back_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        back_desc.SampleDesc.Count = 1;
        back_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* back{};
        ID3D11RenderTargetView* back_rtv{};
        check(SUCCEEDED(gpu.dev->CreateTexture2D(&back_desc, nullptr, &back)) &&
                  SUCCEEDED(gpu.dev->CreateRenderTargetView(back, nullptr, &back_rtv)),
              "jrpg backbuffer");
        gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        gpu.ctx->ResolveSubresource(back, 0, ms_col, 0, DXGI_FORMAT_B8G8R8A8_UNORM);

        weft_view_frame* hframe{};
        check(weft_view_acquire_frame(rt, hud_view.handle, &hframe), "hud acquire");
        const weft_render_scene_frame hsf{sizeof(hsf), WEFT_RENDER_ABI_VERSION, hframe, 0, 0, nullptr};
        weft_render_handle hprep{};
        check(weft_renderer_prepare_scene(renderer, &hsf, &hprep), "hud prepare");
        weft_view_frame_release(hframe);

        weft_render_d3d11_pass hpayload{sizeof(hpayload), WEFT_RENDER_ABI_VERSION, nullptr, back_rtv, nullptr, 0, 0, {}};
        const weft_render_pass hpass{sizeof(hpass), WEFT_RENDER_ABI_VERSION, info.identity, W, H,
                                     WEFT_RENDER_TARGET_LINEAR_BGRA8, 1, &hpayload, sizeof(hpayload)};
        weft_render_handle huse{};
        check(weft_renderer_encode(renderer, hprep, &hpass, &huse), "hud encode");
        weft_renderer_release_prepared(renderer, hprep);
        gpu.ctx->Flush();
        ++serial;
        weft_renderer_notify_submitted(renderer, huse, queue, serial);
        weft_renderer_notify_completed(renderer, info.identity.device_epoch, queue, serial);

        readback_png(gpu.dev, gpu.ctx, back, W, H, out_path);

        weft_renderer_destroy(renderer);
        weft_render_d3d11_destroy(backend);
        weft_view_destroy(rt, hud_view.handle);
        weft_runtime_destroy(rt);
        return 0;
    }

    // =========================================================================
    // MODE 3: 4X Stellaris-Style 3D Solar System + Earth Visualization Overlay
    // =========================================================================
    int run_4x(const std::filesystem::path& page_dir, const std::filesystem::path& out_path) {
        Gpu3D gpu;
        const unsigned W = 1920, H = 1080;

        // 1. Build 3D Solar System Geometry (Sun, Orbital Tracks, Mercury, Venus, Earth + Moon + Selection Ring, Mars, Asteroid Belt, Saturn + Rings)
        MeshBuilder mb;
        const Vec3 sunPos{-4.15f, 0.0f, 1.30f};
        const auto orbitPos = [&](float radius, float angleRad) -> Vec3 {
            return {sunPos.x + std::cos(angleRad) * radius, 0.0f, sunPos.z + std::sin(angleRad) * radius};
        };

        // Sun Sphere (kind = 1.0)
        Material sunMat{{1, 1, 1}, {0, 0, 0}, 1.0f, 0.5f};
        mb.sphere(1.22f, 40, 64, sunMat, Mat4::translate(sunPos.x, sunPos.y, sunPos.z));

        // Orbital Track Rings around the Sun (kind = 0.0 emissive thin torus)
        const Material orbitDim{{0.06f, 0.14f, 0.26f}, {0.08f, 0.20f, 0.36f}, 0.0f, 0.5f};
        const Material orbitEarth{{0.15f, 0.72f, 1.0f}, {0.22f, 0.88f, 1.45f}, 0.0f, 0.5f};
        const float orbitRadii[]{1.98f, 2.95f, 4.30f, 6.15f, 8.20f};
        for (int i = 0; i < 5; ++i) {
            const bool isEarth = (i == 2);
            mb.torus(orbitRadii[i], isEarth ? 0.014f : 0.008f, 160, 6, isEarth ? orbitEarth : orbitDim,
                     Mat4::translate(sunPos.x, 0.0f, sunPos.z));
        }

        // Mercury
        const Vec3 mercPos = orbitPos(1.98f, -0.78f);
        Material mercMat{{0.44f, 0.40f, 0.36f}, {0, 0, 0}, 0.0f, 0.8f};
        mb.sphere(0.17f, 18, 28, mercMat, Mat4::translate(mercPos.x, 0.0f, mercPos.z));

        // Venus (clean shadowed golden-ochre atmosphere, no flat emissive wash)
        const Vec3 venusPos = orbitPos(2.95f, 0.70f);
        Material venusMat{{0.82f, 0.64f, 0.36f}, {0.0f, 0.0f, 0.0f}, 0.0f, 0.7f};
        mb.sphere(0.28f, 24, 36, venusMat, Mat4::translate(venusPos.x, 0.0f, venusPos.z));

        // EARTH (Sol III) — Hero Planet showing ~60% sunlit blue oceans/green continents/clouds + ~40% golden night-side city lights! (kind = 2.0)
        const Vec3 earthPos = orbitPos(4.30f, -0.18f);
        Material earthMat{{0.2f, 0.5f, 0.8f}, {0, 0, 0}, 2.0f, 0.5f};
        mb.sphere(0.95f, 52, 80, earthMat, Mat4::rot_y(0.42f) * Mat4::rot_z(0.22f) * Mat4::translate(earthPos.x, 0.0f, earthPos.z));

        // Glowing Cyan Stellaris Selection Reticle Ring + Gold Orbital Station Ring around Earth
        mb.torus(1.38f, 0.022f, 108, 8, orbitEarth, Mat4::translate(earthPos.x, 0.0f, earthPos.z));
        const Material goldRing{{0.9f, 0.72f, 0.3f}, {0.68f, 0.50f, 0.16f}, 0.0f, 0.5f};
        mb.torus(1.16f, 0.011f, 88, 6, goldRing, Mat4::rot_x(0.14f) * Mat4::translate(earthPos.x, 0.0f, earthPos.z));

        // Moon (Luna) orbiting Earth
        const Vec3 moonPos{earthPos.x + 1.46f, 0.16f, earthPos.z + 0.82f};
        mb.torus(1.68f, 0.007f, 88, 6, orbitDim, Mat4::rot_z(0.08f) * Mat4::translate(earthPos.x, 0.0f, earthPos.z));
        Material moonMat{{0.58f, 0.60f, 0.64f}, {0, 0, 0}, 0.0f, 0.85f};
        mb.sphere(0.21f, 22, 32, moonMat, Mat4::translate(moonPos.x, moonPos.y, moonPos.z));

        // Mars (kind = 3.0) — cleanly separated upper-right-center above Earth
        const Vec3 marsPos = orbitPos(6.15f, -0.78f);
        Material marsMat{{0.6f, 0.2f, 0.1f}, {0, 0, 0}, 3.0f, 0.7f};
        mb.sphere(0.36f, 28, 42, marsMat, Mat4::translate(marsPos.x, 0.0f, marsPos.z));

        // Asteroid Belt between Mars & Saturn (88 tiny faceted rock octahedra)
        const Material rockMat{{0.30f, 0.28f, 0.27f}, {0.02f, 0.02f, 0.03f}, 0.0f, 0.9f};
        for (int k = 0; k < 88; ++k) {
            const float a = -1.18f + float(k) * (2.45f / 88.f);
            const float rr = 6.95f + std::sin(k * 7.3f) * 0.34f;
            const float yy = std::cos(k * 11.7f) * 0.14f;
            const float sz = 0.030f + (k % 4) * 0.012f;
            const Vec3 ap = orbitPos(rr, a);
            mb.octahedron(sz, sz * 0.75f, sz * 1.15f, rockMat,
                          Mat4::rot_y(k * 1.3f) * Mat4::rot_x(k * 0.8f) * Mat4::translate(ap.x, yy, ap.z));
        }

        // Saturn + Wide Tilted Planetary Ring Disk (kind = 4.0 & 5.0) — cleanly upper-left-center!
        const Vec3 satPos = orbitPos(8.20f, -1.46f);
        Material satMat{{0.7f, 0.6f, 0.4f}, {0, 0, 0}, 4.0f, 0.5f};
        mb.sphere(0.60f, 36, 54, satMat, Mat4::translate(satPos.x, 0.0f, satPos.z));
        Material satRingMat{{0.7f, 0.6f, 0.4f}, satPos, 5.0f, 0.5f};
        mb.ring_disk(0.82f, 1.44f, 108, satRingMat,
                     Mat4::rot_x(0.32f) * Mat4::rot_z(-0.26f) * Mat4::translate(satPos.x, 0.0f, satPos.z));

        // 2. Render 3D Solar System (4x MSAA)
        D3D11_TEXTURE2D_DESC ms_desc{};
        ms_desc.Width = W; ms_desc.Height = H;
        ms_desc.MipLevels = ms_desc.ArraySize = 1;
        ms_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        ms_desc.SampleDesc.Count = 4;
        ms_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D* ms_col{};
        ID3D11RenderTargetView* ms_rtv{};
        check(SUCCEEDED(gpu.dev->CreateTexture2D(&ms_desc, nullptr, &ms_col)) &&
                  SUCCEEDED(gpu.dev->CreateRenderTargetView(ms_col, nullptr, &ms_rtv)),
              "4x msaa");

        D3D11_TEXTURE2D_DESC ds_desc = ms_desc;
        ds_desc.Format = DXGI_FORMAT_D32_FLOAT;
        ds_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ID3D11Texture2D* ds_tex{};
        ID3D11DepthStencilView* dsv{};
        check(SUCCEEDED(gpu.dev->CreateTexture2D(&ds_desc, nullptr, &ds_tex)) &&
                  SUCCEEDED(gpu.dev->CreateDepthStencilView(ds_tex, nullptr, &dsv)),
              "4x dsv");

        ID3D11Buffer* vb = gpu.upload_mesh(mb);

        const Vec3 eye{0.85f, 5.55f, 11.55f};
        const Vec3 target{1.15f, -0.25f, -0.55f};
        const Mat4 vpMat = Mat4::look_at_rh(eye, target, {0, 1, 0}) * Mat4::perspective_rh(0.70f, float(W) / float(H), 0.1f, 160.f);

        // Project Sun and planets so we know exact screen-space NDC for corona + debug log for UI alignment!
        const auto project_px = [&](Vec3 p) -> std::array<float, 2> {
            const float x = p.x * vpMat.m[0][0] + p.y * vpMat.m[1][0] + p.z * vpMat.m[2][0] + vpMat.m[3][0];
            const float y = p.x * vpMat.m[0][1] + p.y * vpMat.m[1][1] + p.z * vpMat.m[2][1] + vpMat.m[3][1];
            const float w = p.x * vpMat.m[0][3] + p.y * vpMat.m[1][3] + p.z * vpMat.m[2][3] + vpMat.m[3][3];
            return {(x / w * 0.5f + 0.5f) * 1280.f, (0.5f - y / w * 0.5f) * 720.f};
        };
        const auto sunPx = project_px(sunPos);
        const auto earthPx = project_px(earthPos);
        const auto moonPx = project_px(moonPos);
        const auto marsPx = project_px(marsPos);
        const auto satPx = project_px(satPos);
        std::printf("[4x] projected (1280x720): Sun=(%.1f,%.1f) Earth=(%.1f,%.1f) Moon=(%.1f,%.1f) Mars=(%.1f,%.1f) Saturn=(%.1f,%.1f)\n",
                    sunPx[0], sunPx[1], earthPx[0], earthPx[1], moonPx[0], moonPx[1], marsPx[0], marsPx[1], satPx[0], satPx[1]);

        FrameCB fcb{};
        fcb.viewProj = vpMat;
        fcb.eyePos[0] = eye.x; fcb.eyePos[1] = eye.y; fcb.eyePos[2] = eye.z; fcb.eyePos[3] = 1.f;
        fcb.skyBottom[3] = 2.0f; // 4X Space skybox mode
        // Pass Sun screen NDC in floorGlowA.xy for corona glow, and Sun 3D world pos in floorGlowB.xyz!
        fcb.floorGlowA[0] = (sunPx[0] / 1280.f) * 2.f - 1.f;
        fcb.floorGlowA[1] = 1.f - (sunPx[1] / 720.f) * 2.f;
        fcb.floorGlowB[0] = sunPos.x; fcb.floorGlowB[1] = sunPos.y; fcb.floorGlowB[2] = sunPos.z;

        gpu.ctx->UpdateSubresource(gpu.frame_cb, 0, nullptr, &fcb, 0, 0);
        const float clr[4]{0, 0, 0, 1};
        gpu.ctx->ClearRenderTargetView(ms_rtv, clr);
        gpu.ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
        gpu.ctx->RSSetViewports(1, &vp);
        gpu.ctx->OMSetRenderTargets(1, &ms_rtv, dsv);
        gpu.ctx->OMSetBlendState(gpu.bs_opaque, nullptr, 0xffffffff);
        gpu.ctx->RSSetState(gpu.rs_none);
        gpu.ctx->VSSetConstantBuffers(0, 1, &gpu.frame_cb);
        gpu.ctx->PSSetConstantBuffers(0, 1, &gpu.frame_cb);
        gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Space Nebula + Starfield + Solar Corona Backdrop
        gpu.ctx->OMSetDepthStencilState(gpu.ds_off, 0);
        gpu.ctx->VSSetShader(gpu.sky_vs, nullptr, 0);
        gpu.ctx->PSSetShader(gpu.sky_ps, nullptr, 0);
        gpu.ctx->Draw(3, 0);

        // 3D Solar System Meshes
        gpu.ctx->OMSetDepthStencilState(gpu.ds_write, 0);
        UINT stride = sizeof(Vertex), offset = 0;
        gpu.ctx->IASetInputLayout(gpu.mesh_layout);
        gpu.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        gpu.ctx->VSSetShader(gpu.mesh_vs, nullptr, 0);
        gpu.ctx->PSSetShader(gpu.planet_ps, nullptr, 0);
        gpu.ctx->Draw(UINT(mb.verts.size()), 0);

        // Resolve 4x MSAA 3D Solar System -> Backbuffer
        D3D11_TEXTURE2D_DESC back_desc{};
        back_desc.Width = W; back_desc.Height = H;
        back_desc.MipLevels = back_desc.ArraySize = 1;
        back_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        back_desc.SampleDesc.Count = 1;
        back_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* back{};
        ID3D11RenderTargetView* back_rtv{};
        check(SUCCEEDED(gpu.dev->CreateTexture2D(&back_desc, nullptr, &back)) &&
                  SUCCEEDED(gpu.dev->CreateRenderTargetView(back, nullptr, &back_rtv)),
              "4x backbuffer");
        gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        gpu.ctx->ResolveSubresource(back, 0, ms_col, 0, DXGI_FORMAT_B8G8R8A8_UNORM);

        // 3. Composite Weft 4X Earth Visualization UI (with real backdrop-filter over the 3D Solar System!)
        weft_runtime* rt{};
        const weft_runtime_desc rtd{sizeof(rtd), WEFT_ABI_VERSION, 8, WEFT_RUNTIME_SYSTEM_FONTS};
        check(weft_runtime_create(&rtd, &rt), "runtime");
        const auto view = load_weft_page(rt, page_dir, 1280.f, 720.f, 1.5f);

        constexpr std::uint64_t queue = 1;
        const weft_render_d3d11_desc bd{sizeof(bd), WEFT_RENDER_ABI_VERSION, gpu.dev, queue, 0};
        weft_render_d3d11* backend{};
        weft_render_backend table{};
        check(weft_render_d3d11_create(&bd, &backend, &table), "d3d11_create");
        weft_renderer* renderer{};
        check(weft_renderer_create(&table, nullptr, &renderer), "renderer_create");
        weft_render_info info{};
        info.struct_size = sizeof(info);
        check(weft_renderer_get_info(renderer, &info), "get_info");

        weft_view_frame* frame{};
        check(weft_view_acquire_frame(rt, view.handle, &frame), "acquire");
        const weft_render_scene_frame sf{sizeof(sf), WEFT_RENDER_ABI_VERSION, frame, 0, 0, nullptr};
        weft_render_handle prepared{};
        check(weft_renderer_prepare_scene(renderer, &sf, &prepared), "prepare");
        weft_view_frame_release(frame);

        weft_render_d3d11_pass payload{sizeof(payload), WEFT_RENDER_ABI_VERSION, nullptr, back_rtv, nullptr, 0, 0, {}};
        const weft_render_pass pass{sizeof(pass), WEFT_RENDER_ABI_VERSION, info.identity, W, H,
                                    WEFT_RENDER_TARGET_LINEAR_BGRA8, 1, &payload, sizeof(payload)};
        weft_render_handle use{};
        const weft_render_result enc = weft_renderer_encode(renderer, prepared, &pass, &use);
        if (enc != WEFT_RENDER_OK) {
            weft_renderer_last_failure(renderer, [](void*, const char* t, size_t n) {
                std::fprintf(stderr, "4x encode fail: %.*s\n", int(n), t);
            }, nullptr);
            check(enc, "encode 4x");
        }
        weft_renderer_release_prepared(renderer, prepared);
        gpu.ctx->Flush();
        weft_renderer_notify_submitted(renderer, use, queue, 1);
        weft_renderer_notify_completed(renderer, info.identity.device_epoch, queue, 1);

        readback_png(gpu.dev, gpu.ctx, back, W, H, out_path);

        weft_renderer_destroy(renderer);
        weft_render_d3d11_destroy(backend);
        weft_view_destroy(rt, view.handle);
        weft_runtime_destroy(rt);
        return 0;
    }

    // =========================================================================
    // MODE 4: Tactical FPS HUD over Live D3D11 3D Whitebox / Greybox Arena
    // =========================================================================
    int run_fps(const std::filesystem::path& page_dir, const std::filesystem::path& out_path) {
        Gpu3D gpu;
        const unsigned W = 1920, H = 1080;

        // 1. Build Authentic 3D Whitebox Tactical FPS Level Geometry (Site A Courtyard + Catwalk + Cover + Enemy Dummy + FP Viewmodel)
        MeshBuilder mb;
        // mode (metal channel): 0.0 = world-space prototyping grid, 1.0 = smooth gun/trim, 2.0 = enemy mannequin with crimson rim
        const Material wbFloor{{0.19f, 0.22f, 0.27f}, {0, 0, 0}, 0.0f, 0.85f};
        const Material wbWall{{0.34f, 0.38f, 0.44f}, {0, 0, 0}, 0.0f, 0.82f};
        const Material wbLight{{0.48f, 0.52f, 0.58f}, {0, 0, 0}, 0.0f, 0.78f};
        const Material wbDark{{0.10f, 0.13f, 0.17f}, {0, 0, 0}, 0.0f, 0.72f};
        const Material wbOrange{{0.72f, 0.30f, 0.05f}, {0, 0, 0}, 0.0f, 0.65f};
        const Material wbTeal{{0.06f, 0.36f, 0.44f}, {0, 0, 0}, 0.0f, 0.65f};
        const Material glowCyan{{0.12f, 0.88f, 1.0f}, {0.35f, 1.45f, 1.85f}, 1.0f, 0.25f};
        const Material glowAmber{{1.0f, 0.68f, 0.14f}, {1.25f, 0.72f, 0.10f}, 1.0f, 0.25f};
        const Material enemySuit{{0.045f, 0.052f, 0.075f}, {0, 0, 0}, 2.0f, 0.45f};
        const Material enemyArmor{{0.14f, 0.16f, 0.21f}, {0, 0, 0}, 2.0f, 0.35f};
        const Material enemyVisor{{1.0f, 0.15f, 0.22f}, {2.4f, 0.18f, 0.28f}, 2.0f, 0.15f};
        const Material gunDark{{0.042f, 0.050f, 0.068f}, {0, 0, 0}, 1.0f, 0.30f};
        const Material gunBody{{0.30f, 0.34f, 0.40f}, {0, 0, 0}, 1.0f, 0.26f};

        // Main Courtyard Floor Slab & Site A Raised Platform
        mb.box(24.0f, 0.4f, 46.0f, wbFloor, Mat4::translate(0.0f, -0.2f, -15.0f));
        // Center walkway guide strip (subtle dark concrete lane)
        mb.box(3.4f, 0.02f, 24.0f, wbDark, Mat4::translate(0.0f, 0.01f, -8.5f));
        // Site A Raised Concrete Platform in background
        mb.box(10.5f, 0.60f, 8.5f, wbLight, Mat4::translate(0.0f, 0.30f, -20.5f));
        // Angled front ramp onto Site A platform
        mb.box(4.2f, 0.18f, 2.6f, wbFloor, Mat4::rot_x(0.24f) * Mat4::translate(-0.6f, 0.24f, -15.2f));
        // Site A Orange Perimeter Strip + Objective Beacon Pylon
        mb.box(10.5f, 0.08f, 0.35f, wbOrange, Mat4::translate(0.0f, 0.62f, -16.4f));
        mb.box(1.1f, 2.4f, 1.1f, wbDark, Mat4::translate(-1.35f, 1.80f, -19.2f));
        mb.box(1.18f, 0.16f, 1.18f, glowAmber, Mat4::translate(-1.35f, 2.65f, -19.2f));
        mb.box(0.55f, 0.55f, 0.55f, glowCyan, Mat4::rot_y(0.78f) * Mat4::translate(-1.35f, 3.35f, -19.2f));

        // Left & Right Architectural Courtyard Walls + Structural Pilasters
        mb.box(1.2f, 7.6f, 36.0f, wbWall, Mat4::translate(-7.4f, 3.8f, -12.0f));
        mb.box(1.2f, 7.6f, 36.0f, wbWall, Mat4::translate( 7.4f, 3.8f, -12.0f));
        // Back Arena Retaining Wall + Skyline Silhouette Blocks
        mb.box(18.0f, 6.2f, 1.2f, wbWall, Mat4::translate(0.0f, 3.1f, -26.2f));
        mb.box(5.4f, 11.5f, 4.0f, wbLight, Mat4::translate(-6.8f, 5.75f, -29.5f));
        mb.box(6.2f, 14.0f, 4.5f, wbWall, Mat4::translate( 5.8f, 7.0f, -31.0f));
        mb.box(4.2f, 8.8f, 3.5f, wbLight, Mat4::translate(-0.8f, 4.4f, -32.0f));

        // Wall Pilasters & Architectural Trim Bands along both corridor walls
        for (int k = 0; k < 5; ++k) {
            const float z = -2.5f - k * 5.2f;
            for (int side : {-1, 1}) {
                mb.box(0.65f, 7.6f, 0.90f, wbLight, Mat4::translate(side * 6.55f, 3.8f, z));
                mb.box(0.72f, 0.28f, 0.98f, wbDark, Mat4::translate(side * 6.55f, 2.2f, z));
                mb.box(0.18f, 0.08f, 3.8f, glowCyan, Mat4::translate(side * 6.72f, 3.65f, z - 2.6f));
            }
        }

        // Overhead Industrial Whitebox Skybridge / Catwalk Overpass (at z = -13.5, y = 4.9)
        mb.box(14.8f, 0.55f, 3.2f, wbLight, Mat4::translate(0.0f, 4.85f, -13.5f));
        mb.box(14.8f, 0.95f, 0.24f, wbWall, Mat4::translate(0.0f, 5.55f, -12.02f));
        mb.box(14.8f, 0.14f, 0.30f, wbOrange, Mat4::translate(0.0f, 5.05f, -11.98f));
        // Diagonal bridge support struts
        for (int side : {-1, 1}) {
            mb.box(0.45f, 2.6f, 0.45f, wbDark, Mat4::rot_z(side * 0.62f) * Mat4::translate(side * 5.65f, 3.75f, -13.5f));
        }

        // Left-Side Tactical Whitebox Cover Crates (classic orange & grey dev blocks)
        mb.box(1.85f, 1.32f, 1.65f, wbOrange, Mat4::translate(-2.75f, 0.66f, -5.6f));
        mb.box(1.92f, 0.12f, 1.72f, wbDark, Mat4::translate(-2.75f, 1.34f, -5.6f));
        mb.box(1.45f, 2.35f, 1.45f, wbLight, Mat4::translate(-4.15f, 1.175f, -8.8f));
        mb.box(2.10f, 1.25f, 1.35f, wbTeal, Mat4::translate(-3.45f, 0.625f, -12.8f));

        // Right-Side Tactical Cover Stack (Enemy VANGUARD-07 peeks from behind this!)
        mb.box(1.95f, 1.28f, 1.55f, wbOrange, Mat4::translate(2.55f, 0.64f, -8.9f));
        mb.box(2.02f, 0.12f, 1.62f, wbDark, Mat4::translate(2.55f, 1.30f, -8.9f));
        mb.box(1.55f, 2.45f, 1.65f, wbLight, Mat4::translate(3.95f, 1.225f, -9.8f));
        mb.box(1.80f, 1.45f, 1.80f, wbWall, Mat4::translate(4.65f, 0.725f, -14.8f));
        // Small foreground low cover block on far-left edge for depth parallax
        mb.box(1.40f, 0.95f, 1.20f, wbWall, Mat4::translate(-4.2f, 0.475f, -1.6f));

        // Enemy Tactical Whitebox Combat Mannequin ("VANGUARD-07") peeking left of right cover!
        const Vec3 ePos{1.56f, 0.0f, -8.35f};
        const Mat4 eXf = Mat4::rot_y(-0.34f) * Mat4::translate(ePos.x, ePos.y, ePos.z);
        // Subtle floor contact shadow pad under enemy feet
        mb.box(0.85f, 0.015f, 0.65f, wbDark, Mat4::translate(ePos.x, 0.008f, ePos.z));
        // Legs (combat stance)
        mb.box(0.20f, 0.88f, 0.22f, enemySuit, Mat4::rot_z(0.08f) * Mat4::translate(-0.16f, 0.44f, 0.04f) * eXf);
        mb.box(0.20f, 0.88f, 0.22f, enemySuit, Mat4::rot_z(-0.10f) * Mat4::translate( 0.17f, 0.44f, -0.06f) * eXf);
        // Pelvis & Torso + Chest Armor Vest
        mb.box(0.46f, 0.22f, 0.26f, enemySuit, Mat4::translate(0.0f, 0.92f, 0.0f) * eXf);
        mb.box(0.52f, 0.56f, 0.30f, enemyArmor, Mat4::translate(0.0f, 1.28f, 0.0f) * eXf);
        mb.box(0.34f, 0.34f, 0.34f, enemySuit, Mat4::translate(0.0f, 1.28f, 0.03f) * eXf);
        // Shoulders & Arms aiming forward-left
        mb.box(0.20f, 0.22f, 0.24f, enemyArmor, Mat4::translate(-0.34f, 1.44f, 0.02f) * eXf);
        mb.box(0.20f, 0.22f, 0.24f, enemyArmor, Mat4::translate( 0.34f, 1.44f, -0.02f) * eXf);
        mb.box(0.15f, 0.15f, 0.54f, enemySuit, Mat4::rot_y(0.25f) * Mat4::translate(-0.22f, 1.32f, 0.26f) * eXf);
        mb.box(0.15f, 0.15f, 0.48f, enemySuit, Mat4::rot_y(-0.22f) * Mat4::translate( 0.24f, 1.30f, 0.22f) * eXf);
        // Helmet & Glowing Crimson Tactical Visor
        mb.box(0.27f, 0.30f, 0.29f, enemyArmor, Mat4::translate(0.0f, 1.72f, 0.0f) * eXf);
        mb.box(0.24f, 0.085f, 0.06f, enemyVisor, Mat4::translate(0.0f, 1.73f, 0.15f) * eXf);
        // Enemy Carbine Silhouette
        mb.box(0.08f, 0.12f, 0.92f, enemySuit, Mat4::rot_y(0.12f) * Mat4::translate(-0.04f, 1.30f, 0.48f) * eXf);

        // First-Person Whitebox Tactical Rifle Viewmodel ("VANDAL MK-IV") anchored cleanly in lower-right foreground
        const Vec3 eye{0.0f, 1.68f, 3.80f};
        const Vec3 target{0.35f, 1.56f, -12.0f};
        const Vec3 fwd = norm(target - eye);
        const Vec3 right = norm(cross(fwd, {0, 1, 0}));
        const Vec3 up = cross(right, fwd);
        // Camera-to-world basis transform so viewmodel aligns cleanly with camera POV
        Mat4 camToWorld = Mat4::identity();
        camToWorld.m[0][0] = right.x; camToWorld.m[0][1] = right.y; camToWorld.m[0][2] = right.z;
        camToWorld.m[1][0] = up.x;    camToWorld.m[1][1] = up.y;    camToWorld.m[1][2] = up.z;
        camToWorld.m[2][0] = -fwd.x;  camToWorld.m[2][1] = -fwd.y;  camToWorld.m[2][2] = -fwd.z;
        camToWorld.m[3][0] = eye.x;   camToWorld.m[3][1] = eye.y;   camToWorld.m[3][2] = eye.z;

        const Mat4 gunXf = Mat4::rot_y(0.11f) * Mat4::rot_x(0.02f) * Mat4::translate(0.36f, -0.26f, -0.76f) * camToWorld;
        // Main angular receiver + whitebox titanium upper shroud
        mb.box(0.085f, 0.115f, 0.56f, gunDark, gunXf);
        mb.box(0.092f, 0.055f, 0.48f, gunBody, Mat4::translate(0.0f, 0.045f, -0.05f) * gunXf);
        // Handguard barrel shroud + extended cylindrical barrel & muzzle brake
        mb.box(0.068f, 0.072f, 0.38f, gunBody, Mat4::translate(0.0f, 0.015f, -0.44f) * gunXf);
        mb.prism(8, 0.018f, 0.018f, 0.36f, gunDark, Mat4::rot_x(kPi * 0.5f) * Mat4::translate(0.0f, 0.025f, -0.72f) * gunXf);
        mb.box(0.048f, 0.048f, 0.075f, gunDark, Mat4::translate(0.0f, 0.025f, -0.88f) * gunXf);
        // Cyan energy side rail accent strip (matching VANDAL MK-IV HUD silhouette!)
        mb.box(0.098f, 0.014f, 0.22f, glowCyan, Mat4::translate(0.0f, 0.035f, -0.14f) * gunXf);
        // Reflex Holo-Optic Housing on top rail
        mb.box(0.062f, 0.016f, 0.14f, gunDark, Mat4::translate(0.0f, 0.080f, 0.04f) * gunXf);
        mb.box(0.010f, 0.058f, 0.095f, gunDark, Mat4::translate(-0.028f, 0.112f, 0.04f) * gunXf);
        mb.box(0.010f, 0.058f, 0.095f, gunDark, Mat4::translate( 0.028f, 0.112f, 0.04f) * gunXf);
        mb.box(0.066f, 0.010f, 0.095f, gunDark, Mat4::translate( 0.0f,   0.140f, 0.04f) * gunXf);
        mb.box(0.008f, 0.008f, 0.008f, glowCyan, Mat4::translate( 0.0f,  0.112f, 0.02f) * gunXf);
        // Angled Magazine
        mb.box(0.048f, 0.20f, 0.082f, gunDark, Mat4::rot_x(0.22f) * Mat4::translate(0.0f, -0.13f, -0.14f) * gunXf);

        // 2. Render 3D Whitebox Scene (4x MSAA)
        D3D11_TEXTURE2D_DESC ms_desc{};
        ms_desc.Width = W; ms_desc.Height = H;
        ms_desc.MipLevels = ms_desc.ArraySize = 1;
        ms_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        ms_desc.SampleDesc.Count = 4;
        ms_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D* ms_col{};
        ID3D11RenderTargetView* ms_rtv{};
        check(SUCCEEDED(gpu.dev->CreateTexture2D(&ms_desc, nullptr, &ms_col)) &&
                  SUCCEEDED(gpu.dev->CreateRenderTargetView(ms_col, nullptr, &ms_rtv)),
              "fps msaa");

        D3D11_TEXTURE2D_DESC ds_desc = ms_desc;
        ds_desc.Format = DXGI_FORMAT_D32_FLOAT;
        ds_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ID3D11Texture2D* ds_tex{};
        ID3D11DepthStencilView* dsv{};
        check(SUCCEEDED(gpu.dev->CreateTexture2D(&ds_desc, nullptr, &ds_tex)) &&
                  SUCCEEDED(gpu.dev->CreateDepthStencilView(ds_tex, nullptr, &dsv)),
              "fps dsv");

        ID3D11Buffer* vb = gpu.upload_mesh(mb);

        const Mat4 vpMat = Mat4::look_at_rh(eye, target, {0, 1, 0}) * Mat4::perspective_rh(0.88f, float(W) / float(H), 0.08f, 120.f);
        // Project enemy head marker to exact 1280x720 logical CSS coords for 3D anchor HUD alignment!
        const Vec3 headWorld{ePos.x, 1.94f, ePos.z};
        const float hx = headWorld.x * vpMat.m[0][0] + headWorld.y * vpMat.m[1][0] + headWorld.z * vpMat.m[2][0] + vpMat.m[3][0];
        const float hy = headWorld.x * vpMat.m[0][1] + headWorld.y * vpMat.m[1][1] + headWorld.z * vpMat.m[2][1] + vpMat.m[3][1];
        const float hw = headWorld.x * vpMat.m[0][3] + headWorld.y * vpMat.m[1][3] + headWorld.z * vpMat.m[2][3] + vpMat.m[3][3];
        const float headPxX = (hx / hw * 0.5f + 0.5f) * 1280.f;
        const float headPxY = (0.5f - hy / hw * 0.5f) * 720.f;
        const float distM = std::sqrt(dot(headWorld - eye, headWorld - eye));
        std::printf("[fps] projected enemy head=(%.1f, %.1f) dist=%.1fm\n", headPxX, headPxY, distM);

        FrameCB fcb{};
        fcb.viewProj = vpMat;
        fcb.eyePos[0] = eye.x; fcb.eyePos[1] = eye.y; fcb.eyePos[2] = eye.z; fcb.eyePos[3] = 1.f;
        // Crisp directional sunlight upper-left + cool studio sky fill (balanced so dev-grid stays crisp!)
        fcb.keyDir[0] = -0.58f; fcb.keyDir[1] = 0.72f; fcb.keyDir[2] = 0.38f; fcb.keyDir[3] = 1.55f;
        fcb.keyColor[0] = 1.0f; fcb.keyColor[1] = 0.94f; fcb.keyColor[2] = 0.84f;
        fcb.fillDir[0] = 0.62f; fcb.fillDir[1] = 0.45f; fcb.fillDir[2] = -0.55f; fcb.fillDir[3] = 0.68f;
        fcb.fillColor[0] = 0.40f; fcb.fillColor[1] = 0.58f; fcb.fillColor[2] = 0.88f;
        fcb.fogColor[0] = 0.10f; fcb.fogColor[1] = 0.14f; fcb.fogColor[2] = 0.21f; fcb.fogColor[3] = 0.024f;
        fcb.skyBottom[0] = 0.14f; fcb.skyBottom[1] = 0.19f; fcb.skyBottom[2] = 0.28f; fcb.skyBottom[3] = 3.0f; // FPS whitebox sky
        fcb.skyTop[0] = 0.045f; fcb.skyTop[1] = 0.080f; fcb.skyTop[2] = 0.145f;

        gpu.ctx->UpdateSubresource(gpu.frame_cb, 0, nullptr, &fcb, 0, 0);
        const float clr[4]{0, 0, 0, 1};
        gpu.ctx->ClearRenderTargetView(ms_rtv, clr);
        gpu.ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
        gpu.ctx->RSSetViewports(1, &vp);
        gpu.ctx->OMSetRenderTargets(1, &ms_rtv, dsv);
        gpu.ctx->OMSetBlendState(gpu.bs_opaque, nullptr, 0xffffffff);
        gpu.ctx->RSSetState(gpu.rs_none);
        gpu.ctx->VSSetConstantBuffers(0, 1, &gpu.frame_cb);
        gpu.ctx->PSSetConstantBuffers(0, 1, &gpu.frame_cb);
        gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Draw Whitebox Studio Sky
        gpu.ctx->OMSetDepthStencilState(gpu.ds_off, 0);
        gpu.ctx->IASetInputLayout(nullptr);
        gpu.ctx->VSSetShader(gpu.sky_vs, nullptr, 0);
        gpu.ctx->PSSetShader(gpu.sky_ps, nullptr, 0);
        gpu.ctx->Draw(3, 0);

        // Draw 3D Whitebox Tactical Level + Enemy Dummy + FP Viewmodel
        gpu.ctx->OMSetDepthStencilState(gpu.ds_write, 0);
        UINT stride = sizeof(Vertex), offset = 0;
        gpu.ctx->IASetInputLayout(gpu.mesh_layout);
        gpu.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        gpu.ctx->VSSetShader(gpu.mesh_vs, nullptr, 0);
        gpu.ctx->PSSetShader(gpu.whitebox_ps, nullptr, 0);
        gpu.ctx->Draw(UINT(mb.verts.size()), 0);

        // Resolve 4x MSAA -> Backbuffer
        D3D11_TEXTURE2D_DESC back_desc{};
        back_desc.Width = W; back_desc.Height = H;
        back_desc.MipLevels = back_desc.ArraySize = 1;
        back_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        back_desc.SampleDesc.Count = 1;
        back_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* back{};
        ID3D11RenderTargetView* back_rtv{};
        check(SUCCEEDED(gpu.dev->CreateTexture2D(&back_desc, nullptr, &back)) &&
                  SUCCEEDED(gpu.dev->CreateRenderTargetView(back, nullptr, &back_rtv)),
              "fps backbuffer");
        gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        gpu.ctx->ResolveSubresource(back, 0, ms_col, 0, DXGI_FORMAT_B8G8R8A8_UNORM);

        // 3. Composite Weft Tactical FPS HUD (with real backdrop-filter over 3D Whitebox!)
        weft_runtime* rt{};
        const weft_runtime_desc rtd{sizeof(rtd), WEFT_ABI_VERSION, 8, WEFT_RUNTIME_SYSTEM_FONTS};
        check(weft_runtime_create(&rtd, &rt), "runtime");
        const auto view = load_weft_page(rt, page_dir, 1280.f, 720.f, 1.5f);

        constexpr std::uint64_t queue = 1;
        const weft_render_d3d11_desc bd{sizeof(bd), WEFT_RENDER_ABI_VERSION, gpu.dev, queue, 0};
        weft_render_d3d11* backend{};
        weft_render_backend table{};
        check(weft_render_d3d11_create(&bd, &backend, &table), "d3d11_create");
        weft_renderer* renderer{};
        check(weft_renderer_create(&table, nullptr, &renderer), "renderer_create");
        weft_render_info info{};
        info.struct_size = sizeof(info);
        check(weft_renderer_get_info(renderer, &info), "get_info");

        weft_view_frame* frame{};
        check(weft_view_acquire_frame(rt, view.handle, &frame), "acquire");
        const weft_render_scene_frame sf{sizeof(sf), WEFT_RENDER_ABI_VERSION, frame, 0, 0, nullptr};
        weft_render_handle prepared{};
        check(weft_renderer_prepare_scene(renderer, &sf, &prepared), "prepare");
        weft_view_frame_release(frame);

        weft_render_d3d11_pass payload{sizeof(payload), WEFT_RENDER_ABI_VERSION, nullptr, back_rtv, nullptr, 0, 0, {}};
        const weft_render_pass pass{sizeof(pass), WEFT_RENDER_ABI_VERSION, info.identity, W, H,
                                    WEFT_RENDER_TARGET_LINEAR_BGRA8, 1, &payload, sizeof(payload)};
        weft_render_handle use{};
        const weft_render_result enc = weft_renderer_encode(renderer, prepared, &pass, &use);
        if (enc != WEFT_RENDER_OK) {
            weft_renderer_last_failure(renderer, [](void*, const char* t, size_t n) {
                std::fprintf(stderr, "fps encode fail: %.*s\n", int(n), t);
            }, nullptr);
            check(enc, "encode fps");
        }
        weft_renderer_release_prepared(renderer, prepared);
        gpu.ctx->Flush();
        weft_renderer_notify_submitted(renderer, use, queue, 1);
        weft_renderer_notify_completed(renderer, info.identity.device_epoch, queue, 1);

        readback_png(gpu.dev, gpu.ctx, back, W, H, out_path);

        weft_renderer_destroy(renderer);
        weft_render_d3d11_destroy(backend);
        weft_view_destroy(rt, view.handle);
        weft_runtime_destroy(rt);
        return 0;
    }
} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        std::string mode, page, out;
        for (int i = 1; i + 1 < argc; i += 2) {
            const std::string_view k = argv[i];
            if (k == "--mode") mode = argv[i + 1];
            else if (k == "--page") page = argv[i + 1];
            else if (k == "--out") out = argv[i + 1];
        }
        check(!mode.empty() && !page.empty() && !out.empty(),
              "usage: showcase_host --mode <crpg|jrpg|4x|fps> --page <dir> --out <png>");
        if (mode == "crpg") return run_crpg(utf8_path(page), utf8_path(out));
        if (mode == "jrpg") return run_jrpg(utf8_path(page), utf8_path(out));
        if (mode == "4x") return run_4x(utf8_path(page), utf8_path(out));
        if (mode == "fps") return run_fps(utf8_path(page), utf8_path(out));
        throw Failure("unknown mode: " + mode);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "showcase_host: %s\n", e.what());
        return 1;
    }
}
