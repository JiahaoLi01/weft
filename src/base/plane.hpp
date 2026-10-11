#pragma once

#include "constants/common.hpp"

#include <array>
#include <cmath>

namespace weft::geometry {
    // A projective map of the plane in column-vector layout, m = [a b c; d e f; g h i]: affine transforms and the
    // planes of the perspective subset. Hit testing maps points through inverses of these.
    struct Plane {
        std::array<double, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};

        Plane operator*(const Plane& b) const {
            Plane out;
            out.m.fill(0);
            for (unsigned y = 0; y < 3; ++y) {
                for (unsigned x = 0; x < 3; ++x) {
                    for (unsigned k = 0; k < 3; ++k) {
                        out.m[y * 3 + x] += m[y * 3 + k] * b.m[k * 3 + x];
                    }
                }
            }
            return out;
        }

        // A point mapped through the plane; far away when it maps to infinity.
        std::array<double, 2> point(double x, double y) const {
            const double z = m[6] * x + m[7] * y + m[8];
            if (std::abs(z) < constants::plane_singular_epsilon) {
                return {constants::plane_far_point, constants::plane_far_point};
            }
            return {(m[0] * x + m[1] * y + m[2]) / z, (m[3] * x + m[4] * y + m[5]) / z};
        }

        // False for a singular plane.
        bool inverse(Plane& out) const {
            const auto& a = m;
            const double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) +
                               a[2] * (a[3] * a[7] - a[4] * a[6]);
            if (std::abs(det) < constants::plane_singular_epsilon) {
                return false;
            }
            out.m = {(a[4] * a[8] - a[5] * a[7]) / det, (a[2] * a[7] - a[1] * a[8]) / det,
                     (a[1] * a[5] - a[2] * a[4]) / det, (a[5] * a[6] - a[3] * a[8]) / det,
                     (a[0] * a[8] - a[2] * a[6]) / det, (a[2] * a[3] - a[0] * a[5]) / det,
                     (a[3] * a[7] - a[4] * a[6]) / det, (a[1] * a[6] - a[0] * a[7]) / det,
                     (a[0] * a[4] - a[1] * a[3]) / det};
            return true;
        }

        static Plane translation(double x, double y) {
            return {{1, 0, x, 0, 1, y, 0, 0, 1}};
        }
    };
} // namespace weft::geometry
