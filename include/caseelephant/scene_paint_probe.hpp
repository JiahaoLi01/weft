#pragma once
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace caseelephant {
// Owned API-neutral paint parameters. Strings remain the diagnostic/reference
// scene representation; adapters can consume numeric samples directly.
struct ProbePaintNumber {double absolute{},relative{};friend bool operator==(const ProbePaintNumber&,const ProbePaintNumber&)=default;};
enum class ProbePaintOp {Translate,TranslateX,TranslateY,Scale,ScaleX,ScaleY,Rotate,Matrix,Perspective,RotateY};
struct ProbePaintOperation {
    ProbePaintOp kind{};std::array<ProbePaintNumber,6> args{};unsigned count{};
    friend bool operator==(const ProbePaintOperation&,const ProbePaintOperation&)=default;
};
struct ProbePaintValue {
    enum Kind {Scalar,Color,Transform} kind{};
    ProbePaintNumber scalar;
    std::array<double,4> color{};
    std::vector<ProbePaintOperation> operations;
    friend bool operator==(const ProbePaintValue&,const ProbePaintValue&)=default;
};
struct ProbeScenePaintValues {
    // opacity, transform, background, text color, top/right/bottom/left colors.
    std::array<std::optional<ProbePaintValue>,8> values;
};
const std::array<const char*,8>& scene_paint_properties();
std::optional<ProbePaintValue> parse_scene_paint_value(const std::string&);
ProbePaintValue interpolate_scene_paint_value(const ProbePaintValue&,const ProbePaintValue&,double);
ProbeScenePaintValues compile_scene_paint_values(const std::map<std::string,std::string>&);
}
