#include "vector/path_raster.hpp"

#include "constants/render.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_STROKER_H

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace weft::vector {
    namespace {
        namespace limits = constants::render::path;
        using Point = std::array<double, 2>;
        using Verb = paint::Path::Verb;

        // Coordinates past this many pixels from the mask would overflow FreeType's 26.6 points.
        constexpr double max_coordinate_px = double(1 << 24);

        Point map(const paint::Affine& m, Point p) {
            return {p[0] * m[0] + p[1] * m[2] + m[4], p[0] * m[1] + p[1] * m[3] + m[5]};
        }

        Point point(const std::array<float, 2>& p) {
            return {p[0], p[1]};
        }

        // The most the map's linear part stretches a length: its largest singular value.
        double stretch(const paint::Affine& m) {
            const double a = m[0];
            const double b = m[1];
            const double c = m[2];
            const double d = m[3];
            const double t = a * a + b * b + c * c + d * d;
            const double det = a * d - b * c;
            return std::sqrt(std::max(0.0, (t + std::sqrt(std::max(0.0, t * t - 4 * det * det))) / 2));
        }

        // The farthest a stroke's outline lies from its path, in the path's units: half the width, times the miter
        // limit for miter joins or the diagonal for square caps.
        double reach(const paint::Stroke& s) {
            double factor = 1;
            if (s.join == paint::Stroke::Join::Miter) {
                factor = std::max(factor, double(s.miter_limit));
            }
            if (s.cap == paint::Stroke::Cap::Square) {
                factor = std::max(factor, std::numbers::sqrt2);
            }
            return s.width / 2 * factor;
        }

        // A subpath: its points as a polyline or as the path's own verbs, and whether it closes.
        struct Subpath {
            std::vector<Verb> verbs;
            std::vector<Point> points;
            bool closed{};
        };

        // The path's subpaths. A verb after Close starts a new subpath at the closed one's start.
        std::vector<Subpath> subpaths(const paint::Path& path) {
            std::vector<Subpath> out;
            std::size_t p{};
            Point start{};
            bool open{};
            for (const auto verb : path.verbs) {
                if (verb == Verb::Move) {
                    start = point(path.points[p++]);
                    out.push_back({{Verb::Move}, {start}, false});
                    open = true;
                    continue;
                }
                if (!open) {
                    out.push_back({{Verb::Move}, {start}, false});
                    open = true;
                }
                auto& s = out.back();
                if (verb == Verb::Close) {
                    s.closed = true;
                    open = false;
                    continue;
                }
                const std::size_t count = verb == Verb::Cubic ? 3 : 1;
                s.verbs.push_back(verb);
                for (std::size_t i = 0; i < count; ++i) {
                    s.points.push_back(point(path.points[p++]));
                }
            }
            return out;
        }

        // Appends a cubic's points, flattened within tolerance by subdivision, to a polyline (its start excluded).
        void flatten(Point p0, Point c1, Point c2, Point p, double tolerance, int depth, std::vector<Point>& out) {
            const double dx = p[0] - p0[0];
            const double dy = p[1] - p0[1];
            const double length = std::hypot(dx, dy);
            const auto distance = [&](Point q) {
                return length > 0 ? std::abs((q[0] - p0[0]) * dy - (q[1] - p0[1]) * dx) / length
                                  : std::hypot(q[0] - p0[0], q[1] - p0[1]);
            };
            if (depth >= limits::max_flattening_depth || std::max(distance(c1), distance(c2)) <= tolerance) {
                out.push_back(p);
                return;
            }
            const Point a{(p0[0] + c1[0]) / 2, (p0[1] + c1[1]) / 2};
            const Point b{(c1[0] + c2[0]) / 2, (c1[1] + c2[1]) / 2};
            const Point c{(c2[0] + p[0]) / 2, (c2[1] + p[1]) / 2};
            const Point ab{(a[0] + b[0]) / 2, (a[1] + b[1]) / 2};
            const Point bc{(b[0] + c[0]) / 2, (b[1] + c[1]) / 2};
            const Point mid{(ab[0] + bc[0]) / 2, (ab[1] + bc[1]) / 2};
            flatten(p0, a, ab, mid, tolerance, depth + 1, out);
            flatten(mid, bc, c, p, tolerance, depth + 1, out);
        }

        // A subpath as a polyline, closed back to its start when it closes.
        std::vector<Point> polyline(const Subpath& s, double tolerance) {
            std::vector<Point> out{s.points[0]};
            std::size_t p = 1;
            for (std::size_t i = 1; i < s.verbs.size(); ++i) {
                if (s.verbs[i] == Verb::Cubic) {
                    flatten(out.back(), s.points[p], s.points[p + 1], s.points[p + 2], tolerance, 0, out);
                    p += 3;
                } else {
                    out.push_back(s.points[p++]);
                }
            }
            if (s.closed) {
                out.push_back(s.points[0]);
            }
            return out;
        }

        // The dashes of a polyline as open polylines: on and off lengths from pattern, starting offset into it.
        // A dash of no length keeps the direction it lies in, for caps.
        bool dash(const std::vector<Point>& line, const std::vector<double>& pattern, double offset,
                  std::vector<std::vector<Point>>& out) {
            double total{};
            for (auto v : pattern) {
                total += v;
            }
            std::size_t index{};
            double left = 0;
            // Position into the pattern.
            double phase = std::fmod(offset, total);
            if (phase < 0) {
                phase += total;
            }
            // A dash of no length at the phase stays current, so a dotted pattern starts with a dot.
            while (phase > pattern[index] || (phase == pattern[index] && pattern[index] > 0)) {
                phase -= pattern[index];
                index = (index + 1) % pattern.size();
            }
            left = pattern[index] - phase;
            bool on = index % 2 == 0;
            std::vector<Point> current;
            if (on) {
                current.push_back(line[0]);
            }
            for (std::size_t i = 1; i < line.size(); ++i) {
                Point a = line[i - 1];
                const Point b = line[i];
                double length = std::hypot(b[0] - a[0], b[1] - a[1]);
                while (length > 0 && left <= length) {
                    const double t = left / length;
                    const Point cut{a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t};
                    if (on) {
                        current.push_back(cut);
                        if (current.size() == 1) {
                            // A dash of no length: a sliver along the segment, so caps have a direction.
                            current.push_back(cut);
                        }
                        out.push_back(std::move(current));
                        current.clear();
                        if (out.size() > std::size_t(limits::max_dash_segments)) {
                            return false;
                        }
                    } else {
                        current = {cut};
                    }
                    on = !on;
                    index = (index + 1) % pattern.size();
                    a = cut;
                    length -= left;
                    left = pattern[index];
                }
                left -= length;
                if (on) {
                    current.push_back(b);
                }
            }
            // A dash that starts at the end has no length; it shows only when the pattern asks for one of no length.
            if (on && current.size() > 1 && (left < pattern[index] || pattern[index] == 0)) {
                out.push_back(std::move(current));
            }
            return true;
        }

        // An FT_Outline being built: points in 26.6, tags and contour ends.
        struct Outline {
            std::vector<FT_Vector> points;
            std::vector<unsigned char> tags;
            std::vector<unsigned short> contours;
            bool open{};
            bool overflow{};

            FT_Vector fixed(Point p) {
                if (!(std::abs(p[0]) < max_coordinate_px) || !(std::abs(p[1]) < max_coordinate_px)) {
                    overflow = true;
                    return {};
                }
                return {FT_Pos(std::lround(p[0] * limits::freetype_subpixels)),
                        FT_Pos(std::lround(p[1] * limits::freetype_subpixels))};
            }

            void move(Point p) {
                end();
                points.push_back(fixed(p));
                tags.push_back(FT_CURVE_TAG_ON);
                open = true;
            }

            void line(Point p) {
                points.push_back(fixed(p));
                tags.push_back(FT_CURVE_TAG_ON);
            }

            void cubic(Point c1, Point c2, Point p) {
                points.push_back(fixed(c1));
                tags.push_back(FT_CURVE_TAG_CUBIC);
                points.push_back(fixed(c2));
                tags.push_back(FT_CURVE_TAG_CUBIC);
                line(p);
            }

            void end() {
                if (open) {
                    contours.push_back((unsigned short)(points.size() - 1));
                    open = false;
                }
            }

            // A point already in 26.6.
            void line_fixed(FT_Vector p) {
                points.push_back(p);
                tags.push_back(FT_CURVE_TAG_ON);
            }

            bool fits() const {
                return !overflow && points.size() <= FT_OUTLINE_POINTS_MAX &&
                       contours.size() <= FT_OUTLINE_CONTOURS_MAX;
            }

            FT_Outline view(int flags) {
                end();
                FT_Outline o{};
                o.n_points = (unsigned short)points.size();
                o.n_contours = (unsigned short)contours.size();
                o.points = points.data();
                o.tags = tags.data();
                o.contours = contours.data();
                o.flags = flags;
                return o;
            }
        };

        // An outline's curves as lines within limits::raster_flattening_tolerance pixels. FreeType's rasterizer
        // flattens curves within about a sixth of a pixel, which shrinks a 10 px disc by 0.7%.
        Outline flattened(FT_Outline& source) {
            struct Builder {
                Outline out;
                FT_Vector last{};

                void cubic(Point c1, Point c2, Point to) {
                    std::vector<Point> line;
                    const Point from{double(last.x), double(last.y)};
                    flatten(from, c1, c2, to, limits::raster_flattening_tolerance * limits::freetype_subpixels, 0,
                            line);
                    for (const auto& p : line) {
                        out.line_fixed({FT_Pos(std::lround(p[0])), FT_Pos(std::lround(p[1]))});
                    }
                }
            } b;

            FT_Outline_Funcs funcs{};
            funcs.move_to = [](const FT_Vector* to, void* user) {
                auto& b = *static_cast<Builder*>(user);
                b.out.end();
                b.out.line_fixed(*to);
                b.out.open = true;
                b.last = *to;
                return 0;
            };
            funcs.line_to = [](const FT_Vector* to, void* user) {
                auto& b = *static_cast<Builder*>(user);
                b.out.line_fixed(*to);
                b.last = *to;
                return 0;
            };
            funcs.conic_to = [](const FT_Vector* control, const FT_Vector* to, void* user) {
                auto& b = *static_cast<Builder*>(user);
                const Point p0{double(b.last.x), double(b.last.y)};
                const Point c{double(control->x), double(control->y)};
                const Point p{double(to->x), double(to->y)};
                b.cubic({p0[0] + 2.0 / 3 * (c[0] - p0[0]), p0[1] + 2.0 / 3 * (c[1] - p0[1])},
                        {p[0] + 2.0 / 3 * (c[0] - p[0]), p[1] + 2.0 / 3 * (c[1] - p[1])}, p);
                b.last = *to;
                return 0;
            };
            funcs.cubic_to = [](const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user) {
                auto& b = *static_cast<Builder*>(user);
                b.cubic({double(c1->x), double(c1->y)}, {double(c2->x), double(c2->y)}, {double(to->x), double(to->y)});
                b.last = *to;
                return 0;
            };
            FT_Outline_Decompose(&source, &funcs, &b);
            b.out.end();
            return std::move(b.out);
        }

        // Whether two contours of an outline may cover one pixel together: their bounds meet, and they wind the same
        // way (under even-odd, either way). FreeType's coverage of a pixel adds what each edge covers of it, so where
        // nonzero winding counts a pixel's area once (two coincident edges of one direction: KaTeX's square root draws
        // its bar twice), the sum counts it twice; even-odd folds it. Contours of no area wind neither way.
        bool overlapping(const FT_Outline& o, bool evenodd) {
            struct Contour {
                FT_BBox box;
                int winding;
            };

            std::vector<Contour> contours;
            std::size_t start{};
            for (std::size_t c = 0; c < o.n_contours; ++c) {
                const std::size_t end = o.contours[c];
                FT_BBox box{o.points[start].x, o.points[start].y, o.points[start].x, o.points[start].y};
                double area{};
                for (auto i = start; i <= end; ++i) {
                    const auto& p = o.points[i];
                    const auto& q = o.points[i == end ? start : i + 1];
                    area += double(p.x) * double(q.y) - double(q.x) * double(p.y);
                    box.xMin = std::min(box.xMin, p.x);
                    box.yMin = std::min(box.yMin, p.y);
                    box.xMax = std::max(box.xMax, p.x);
                    box.yMax = std::max(box.yMax, p.y);
                }
                if (area != 0) {
                    contours.push_back({box, area > 0 ? 1 : -1});
                }
                start = end + 1;
            }
            std::sort(contours.begin(), contours.end(), [](const Contour& a, const Contour& b) {
                return a.box.xMin < b.box.xMin;
            });
            for (std::size_t i = 0; i < contours.size(); ++i) {
                const auto& a = contours[i];
                for (auto j = i + 1; j < contours.size() && contours[j].box.xMin < a.box.xMax; ++j) {
                    const auto& b = contours[j];
                    if (b.box.yMin < a.box.yMax && a.box.yMin < b.box.yMax && (evenodd || a.winding == b.winding)) {
                        return true;
                    }
                }
            }
            return false;
        }

        // Spans of an outline drawn overlap_subpixels times larger: each subpixel's coverage, averaged into its
        // pixel's row of the mask (ft_smooth_overlap_spans). A pixel's subpixels add up to 256, held at 255.
        struct Oversampled {
            std::uint8_t* rows{};
            std::uint32_t width{};
        };

        void oversampled_spans(int y, int count, const FT_Span* spans, void* user) {
            const auto& target = *static_cast<const Oversampled*>(user);
            constexpr unsigned subpixels = limits::overlap_subpixels * limits::overlap_subpixels;
            auto* row = target.rows + std::size_t(y / limits::overlap_subpixels) * target.width;
            for (; count--; ++spans) {
                const unsigned cover = (spans->coverage + subpixels / 2) / subpixels;
                for (unsigned x = 0; x < spans->len; ++x) {
                    auto& pixel = row[(spans->x + x) / limits::overlap_subpixels];
                    const unsigned sum = pixel + cover;
                    pixel = std::uint8_t(sum - (sum >> 8));
                }
            }
        }
    } // namespace

    std::array<std::int32_t, 4> mask_bounds(const paint::PathPaint& paint, const paint::Affine& device,
                                            const std::array<std::int32_t, 4>& clip) {
        if (!paint.path || paint.path->points.empty() || (paint.stroke && paint.stroke->width <= 0)) {
            return {};
        }
        constexpr double infinite = std::numeric_limits<double>::infinity();
        double left = infinite;
        double top = infinite;
        double right = -infinite;
        double bottom = -infinite;
        for (const auto& p : paint.path->points) {
            const auto q = map(device, point(p));
            left = std::min(left, q[0]);
            top = std::min(top, q[1]);
            right = std::max(right, q[0]);
            bottom = std::max(bottom, q[1]);
        }
        const double grow = (paint.stroke ? reach(*paint.stroke) * stretch(device) : 0) + limits::antialias_margin;
        const auto whole = [](double v) {
            return std::int32_t(std::clamp(v, -max_coordinate_px, max_coordinate_px));
        };
        const std::array<std::int32_t, 4> b{
            std::max(clip[0], whole(std::floor(left - grow))), std::max(clip[1], whole(std::floor(top - grow))),
            std::min(clip[2], whole(std::ceil(right + grow))), std::min(clip[3], whole(std::ceil(bottom + grow)))};
        if (b[2] <= b[0] || b[3] <= b[1]) {
            return {};
        }
        return b;
    }

    struct Rasterizer::Impl {
        FT_Library library{};
        FT_Stroker stroker{};

        Impl() {
            if (FT_Init_FreeType(&library)) {
                throw std::runtime_error("WEFT_VECTOR: FreeType initialization failed");
            }
            if (FT_Stroker_New(library, &stroker)) {
                FT_Done_FreeType(library);
                throw std::runtime_error("WEFT_VECTOR: FreeType stroker allocation failed");
            }
        }

        ~Impl() {
            FT_Stroker_Done(stroker);
            FT_Done_FreeType(library);
        }

        // The outline of a stroked path in device pixels relative to origin. The stroker works in the path's units
        // scaled by k, close to device pixels, for the precision of 26.6 points.
        Status stroke(const paint::Path& path, const paint::Stroke& s, const paint::Affine& device, Point origin,
                      Outline& out) {
            const double k = std::max(stretch(device), 1.0 / limits::freetype_subpixels);
            const auto scaled = [&](Point p) {
                return FT_Vector{FT_Pos(std::lround(p[0] * k * limits::freetype_subpixels)),
                                 FT_Pos(std::lround(p[1] * k * limits::freetype_subpixels))};
            };
            for (const auto& p : path.points) {
                if (!(std::abs(p[0] * k) < max_coordinate_px) || !(std::abs(p[1] * k) < max_coordinate_px)) {
                    return make_diagnostic(Diag::PaintList, "path coordinate bound");
                }
            }
            const auto cap = s.cap == paint::Stroke::Cap::Round    ? FT_STROKER_LINECAP_ROUND
                             : s.cap == paint::Stroke::Cap::Square ? FT_STROKER_LINECAP_SQUARE
                                                                   : FT_STROKER_LINECAP_BUTT;
            const auto join = s.join == paint::Stroke::Join::Round   ? FT_STROKER_LINEJOIN_ROUND
                              : s.join == paint::Stroke::Join::Bevel ? FT_STROKER_LINEJOIN_BEVEL
                                                                     : FT_STROKER_LINEJOIN_MITER_FIXED;
            FT_Stroker_Rewind(stroker);
            FT_Stroker_Set(stroker, FT_Fixed(std::lround(s.width / 2 * k * limits::freetype_subpixels)), cap, join,
                           FT_Fixed(std::lround(double(s.miter_limit) * limits::freetype_fixed_one)));
            // An open polyline; one of no length keeps a sliver along x, so round and square caps still draw.
            const auto open_line = [&](const std::vector<Point>& line) {
                auto first = scaled(line[0]);
                const bool moved = std::any_of(line.begin() + 1, line.end(), [&](Point p) {
                    const auto to = scaled(p);
                    return to.x != first.x || to.y != first.y;
                });
                if (!moved && s.cap == paint::Stroke::Cap::Butt) {
                    return;
                }
                FT_Stroker_BeginSubPath(stroker, &first, true);
                if (moved) {
                    for (std::size_t i = 1; i < line.size(); ++i) {
                        auto to = scaled(line[i]);
                        FT_Stroker_LineTo(stroker, &to);
                    }
                } else {
                    FT_Vector to{first.x + 1, first.y};
                    FT_Stroker_LineTo(stroker, &to);
                }
                FT_Stroker_EndSubPath(stroker);
            };
            std::vector<double> pattern(s.dashes.begin(), s.dashes.end());
            if (pattern.size() % 2) {
                pattern.insert(pattern.end(), s.dashes.begin(), s.dashes.end());
            }
            double total{};
            for (auto v : pattern) {
                total += v;
            }
            const double tolerance = limits::dash_flattening_tolerance / std::max(stretch(device), 1e-9);
            for (const auto& sub : subpaths(path)) {
                if (total > 0) {
                    std::vector<std::vector<Point>> dashes;
                    if (!dash(polyline(sub, tolerance), pattern, s.dash_offset, dashes)) {
                        return make_diagnostic(Diag::PaintList, "path dash bound");
                    }
                    for (const auto& d : dashes) {
                        open_line(d);
                    }
                    continue;
                }
                // Points that meet in the stroker's 26.6 units make a subpath of no length.
                const auto first = scaled(sub.points[0]);
                if (sub.verbs.size() == 1 || std::all_of(sub.points.begin(), sub.points.end(), [&](Point p) {
                        const auto q = scaled(p);
                        return q.x == first.x && q.y == first.y;
                    })) {
                    // A subpath of no length shows only as round or square caps.
                    if (s.cap != paint::Stroke::Cap::Butt) {
                        open_line({sub.points[0], sub.points[0]});
                    }
                    continue;
                }
                auto start = first;
                FT_Stroker_BeginSubPath(stroker, &start, !sub.closed);
                std::size_t p = 1;
                for (std::size_t i = 1; i < sub.verbs.size(); ++i) {
                    if (sub.verbs[i] == Verb::Cubic) {
                        auto c1 = scaled(sub.points[p]);
                        auto c2 = scaled(sub.points[p + 1]);
                        auto to = scaled(sub.points[p + 2]);
                        FT_Stroker_CubicTo(stroker, &c1, &c2, &to);
                        p += 3;
                    } else {
                        auto to = scaled(sub.points[p++]);
                        FT_Stroker_LineTo(stroker, &to);
                    }
                }
                FT_Stroker_EndSubPath(stroker);
            }
            FT_UInt points{};
            FT_UInt contours{};
            if (FT_Stroker_GetCounts(stroker, &points, &contours) || points > FT_OUTLINE_POINTS_MAX ||
                contours > FT_OUTLINE_CONTOURS_MAX) {
                return make_diagnostic(Diag::PaintList, "path stroke outline bound");
            }
            std::vector<FT_Vector> stroked(points);
            std::vector<unsigned char> tags(points);
            std::vector<unsigned short> ends(contours);
            FT_Outline exported{};
            exported.points = stroked.data();
            exported.tags = tags.data();
            exported.contours = ends.data();
            FT_Stroker_Export(stroker, &exported);
            // Back to the path's units, then through the map into device pixels relative to the mask.
            const auto unit = [&](const FT_Vector& v) {
                return Point{double(v.x) / limits::freetype_subpixels / k,
                             double(v.y) / limits::freetype_subpixels / k};
            };
            std::size_t start{};
            for (std::size_t c = 0; c < exported.n_contours; ++c) {
                const std::size_t end = exported.contours[c];
                for (std::size_t i = start; i <= end; ++i) {
                    const auto q = map(device, unit(exported.points[i]));
                    out.points.push_back(out.fixed({q[0] - origin[0], q[1] - origin[1]}));
                    out.tags.push_back(FT_CURVE_TAG(exported.tags[i]));
                }
                out.contours.push_back((unsigned short)(out.points.size() - 1));
                start = end + 1;
            }
            return {};
        }
    };

    Rasterizer::Rasterizer() : impl_(std::make_unique<Impl>()) {
    }

    Rasterizer::~Rasterizer() = default;

    Result<Mask> Rasterizer::rasterize(const paint::PathPaint& paint, const paint::Affine& device,
                                       const std::array<std::int32_t, 4>& bounds) {
        Mask mask;
        if (!paint.path || bounds[2] <= bounds[0] || bounds[3] <= bounds[1] ||
            (paint.stroke && paint.stroke->width <= 0)) {
            return mask;
        }
        const auto width = std::uint64_t(std::int64_t(bounds[2]) - bounds[0]);
        const auto height = std::uint64_t(std::int64_t(bounds[3]) - bounds[1]);
        if (width * height > std::uint64_t(limits::max_mask_pixels)) {
            return make_diagnostic(Diag::PaintList, "path mask bound");
        }
        mask.x = bounds[0];
        mask.y = bounds[1];
        mask.width = std::uint32_t(width);
        mask.height = std::uint32_t(height);
        mask.coverage.assign(std::size_t(width * height), 0);
        const Point origin{double(mask.x), double(mask.y)};
        Outline outline;
        int flags = FT_OUTLINE_NONE;
        if (paint.stroke) {
            if (auto status = impl_->stroke(*paint.path, *paint.stroke, device, origin, outline); !status) {
                return status.error();
            }
        } else {
            const auto local = [&](const std::array<float, 2>& p) {
                const auto q = map(device, point(p));
                return Point{q[0] - origin[0], q[1] - origin[1]};
            };
            if (paint.path->evenodd) {
                flags |= FT_OUTLINE_EVEN_ODD_FILL;
            }
            const auto& path = *paint.path;
            std::size_t p{};
            Point start{};
            bool open{};
            for (const auto verb : path.verbs) {
                switch (verb) {
                    case Verb::Move:
                        start = local(path.points[p++]);
                        outline.move(start);
                        open = true;
                        break;
                    case Verb::Line:
                        if (!open) {
                            outline.move(start);
                            open = true;
                        }
                        outline.line(local(path.points[p++]));
                        break;
                    case Verb::Cubic:
                        if (!open) {
                            outline.move(start);
                            open = true;
                        }
                        outline.cubic(local(path.points[p]), local(path.points[p + 1]), local(path.points[p + 2]));
                        p += 3;
                        break;
                    case Verb::Close:
                        outline.end();
                        open = false;
                        break;
                }
            }
        }
        if (!outline.fits()) {
            return make_diagnostic(Diag::PaintList, "path outline bound");
        }
        auto curved = outline.view(flags);
        if (curved.n_points == 0) {
            return mask;
        }
        auto lines = flattened(curved);
        if (!lines.fits()) {
            return make_diagnostic(Diag::PaintList, "path outline bound");
        }
        auto source = lines.view(flags);
        // Overlapping contours of a fill: the outline overlap_subpixels times larger, its spans averaged into the
        // mask (FreeType's rendering of FT_OUTLINE_OVERLAP outlines), where its spans' x and points reach.
        if (!paint.stroke && overlapping(source, paint.path->evenodd) &&
            mask.width * limits::overlap_subpixels <= unsigned(limits::freetype_max_span_x)) {
            FT_BBox box;
            FT_Outline_Get_CBox(&source, &box);
            constexpr long reach = limits::freetype_max_outline_coordinate / limits::overlap_subpixels;
            if (box.xMin > -reach && box.yMin > -reach && box.xMax < reach && box.yMax < reach) {
                for (auto& p : lines.points) {
                    p.x *= limits::overlap_subpixels;
                    p.y *= limits::overlap_subpixels;
                }
                auto larger = lines.view(flags);
                Oversampled target{mask.coverage.data(), mask.width};
                FT_Raster_Params params{};
                params.flags = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
                params.gray_spans = oversampled_spans;
                params.user = &target;
                params.clip_box = {0, 0, FT_Pos(mask.width) * limits::overlap_subpixels,
                                   FT_Pos(mask.height) * limits::overlap_subpixels};
                if (FT_Outline_Render(impl_->library, &larger, &params)) {
                    return make_diagnostic(Diag::PaintList, "path raster");
                }
                return mask;
            }
        }
        // Rows top-down: a negative pitch puts FreeType's y = 0 row first, and outline y grows down like device y.
        FT_Bitmap target{};
        target.rows = mask.height;
        target.width = mask.width;
        target.pitch = -int(mask.width);
        target.buffer = mask.coverage.data();
        target.num_grays = 256;
        target.pixel_mode = FT_PIXEL_MODE_GRAY;
        FT_Raster_Params params{};
        params.target = &target;
        params.flags = FT_RASTER_FLAG_AA;
        if (FT_Outline_Render(impl_->library, &source, &params)) {
            return make_diagnostic(Diag::PaintList, "path raster");
        }
        return mask;
    }
} // namespace weft::vector
