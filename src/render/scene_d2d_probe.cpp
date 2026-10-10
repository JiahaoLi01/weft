#define NOMINMAX
#include "caseelephant/scene_probe.hpp"
#include <d2d1_1.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <numbers>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace caseelephant {namespace {
using Microsoft::WRL::ComPtr;using Style=ProbeSceneStyle;constexpr float pi=std::numbers::pi_v<float>;
[[noreturn]] void fail(const std::string& s){throw std::invalid_argument("CE_SCENE_RASTER: "+s);}
void check(HRESULT h){if(FAILED(h))throw std::runtime_error("CE_SCENE_D2D: HRESULT="+std::to_string(static_cast<unsigned long>(h)));}
std::string get(const Style& s,const std::string& k,const char* fallback=""){auto i=s.find(k);return i==s.end()?fallback:i->second;}
std::string trim(std::string s){auto a=s.find_first_not_of(" \t\r\n");return a==s.npos?"":s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);}
float num(std::string s){std::size_t p{};float n=std::stof(s,&p);if(!std::isfinite(n) || p!=s.size() || std::abs(n)>1e7)fail("numeric value "+s);return n;}
float len(std::string s,float reference=1){s=trim(s);if(s.starts_with("calc(")){const auto plus=s.find(" + ");if(plus==s.npos)fail("mixed length calc");return len(s.substr(5,plus-5),reference)+len(s.substr(plus+3,s.size()-plus-4),reference);}if(s.ends_with('%'))return num(s.substr(0,s.size()-1))*reference/100;if(s.ends_with("px") || s.ends_with("em")){bool em=s.ends_with("em");s.resize(s.size()-2);return num(s)*(em?reference:1);}return num(s);}
std::vector<std::string> split(std::string_view s,char delimiter=' '){std::vector<std::string> out;std::size_t start{};int depth{};for(std::size_t p=0;p<s.size();++p){if(s[p]=='(')++depth;else if(s[p]==')')--depth;else if(s[p]==delimiter && !depth){auto v=trim(std::string(s.substr(start,p-start)));if(!v.empty())out.push_back(v);start=p+1;}}auto v=trim(std::string(s.substr(start)));if(!v.empty())out.push_back(v);return out;}
D2D1_COLOR_F color(std::string s,const Style& style={}){s=trim(s);if(s=="none" || s=="transparent")return {0,0,0,0};if(s=="currentColor" || s=="currentcolor")s=get(style,"color","#000000");if(s=="black")s="#000000";if(s=="white")s="#ffffff";if(s.size()==4 && s[0]=='#')s=std::string("#")+s[1]+s[1]+s[2]+s[2]+s[3]+s[3];auto c=parse_probe_css_color(s);return {c[0],c[1],c[2],c[3]};}
std::wstring utf16(std::string_view text){if(text.empty())return {};int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);if(!count)fail("invalid UTF-8");std::wstring out(std::size_t(count),L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),out.data(),count);return out;}
struct Scanner {
    std::string value;std::size_t p{};
    void spaces(){while(p<value.size() && (std::isspace(static_cast<unsigned char>(value[p])) || value[p]==','))++p;}
    bool end(){spaces();return p==value.size();}
    bool has_number(){spaces();return p<value.size() && (std::isdigit(static_cast<unsigned char>(value[p])) || value[p]=='+' || value[p]=='-' || value[p]=='.');}
    float number(){spaces();char* end{};const auto v=std::strtof(value.c_str()+p,&end);if(end==value.c_str()+p || !std::isfinite(v))fail("path number near "+value.substr(p,40));p=std::size_t(end-value.c_str());return v;}
};
// D2D row-vector matrices. CSS transform functions multiply in listed order
// for column vectors, hence prepend each local operation here.
D2D1::Matrix3x2F transform(std::string s,float w,float h,bool svg){
    auto matrix=D2D1::Matrix3x2F::Identity();std::size_t p{};
    if(s.empty() || s=="none")return matrix;
    while(p<s.size()){while(p<s.size() && std::isspace(static_cast<unsigned char>(s[p])))++p;if(p==s.size())break;auto b=s.find('(',p),e=s.find(')',b);if(b==s.npos || e==s.npos)fail("transform syntax");auto name=trim(s.substr(p,b-p));auto args=s.substr(b+1,e-b-1);std::replace(args.begin(),args.end(),',',' ');auto a=split(args);auto n=[&](unsigned i,float ref=1){return i<a.size()?len(a[i],ref):0;};auto op=D2D1::Matrix3x2F::Identity();
        if(name=="translate" || name=="translateX" || name=="translateY")op=D2D1::Matrix3x2F::Translation(name=="translateY"?0:n(0,w),name=="translateY"?n(0,h):name=="translateX"?0:n(1,h));
        else if(name=="scale" || name=="scaleX" || name=="scaleY"){float x=name=="scaleY"?1:n(0),y=name=="scaleX"?1:name=="scaleY"?n(0):a.size()>1?n(1):x;op=D2D1::Matrix3x2F::Scale(x,y);}
        else if(name=="rotate"){auto v=a.at(0);float angle=v.ends_with("deg")?num(v.substr(0,v.size()-3)):len(v)*180/pi;op=D2D1::Matrix3x2F::Rotation(angle,svg && a.size()>2?D2D1::Point2F(n(1),n(2)):D2D1::Point2F());}
        else if(name=="matrix"){if(a.size()!=6)fail("matrix arity");op={n(0),n(1),n(2),n(3),n(4),n(5)};}
        else if(name=="perspective" || name=="rotateY")fail("projective transform requires layer");
        else fail("unsupported transform "+name);
        matrix=op*matrix;p=e+1;
    }return matrix;
}
D2D1::Matrix3x2F transform(const ProbePaintValue& value,float w,float h){
    auto matrix=D2D1::Matrix3x2F::Identity();
    for(const auto& a:value.operations){auto n=[&](unsigned i,float ref=1){return i<a.count?float(a.args[i].absolute+a.args[i].relative*ref):0.f;};auto op=D2D1::Matrix3x2F::Identity();
        switch(a.kind){
        case ProbePaintOp::Translate:op=D2D1::Matrix3x2F::Translation(n(0,w),n(1,h));break;
        case ProbePaintOp::TranslateX:op=D2D1::Matrix3x2F::Translation(n(0,w),0);break;
        case ProbePaintOp::TranslateY:op=D2D1::Matrix3x2F::Translation(0,n(0,h));break;
        case ProbePaintOp::Scale:op=D2D1::Matrix3x2F::Scale(n(0),a.count>1?n(1):n(0));break;
        case ProbePaintOp::ScaleX:op=D2D1::Matrix3x2F::Scale(n(0),1);break;
        case ProbePaintOp::ScaleY:op=D2D1::Matrix3x2F::Scale(1,n(0));break;
        case ProbePaintOp::Rotate:op=D2D1::Matrix3x2F::Rotation(n(0));break;
        case ProbePaintOp::Matrix:op={n(0),n(1),n(2),n(3),n(4),n(5)};break;
        default:fail("projective transform requires layer");
        }matrix=op*matrix;
    }return matrix;
}
std::array<float,2> origin(const Style& s,float w,float h){auto v=get(s,"transform-origin","50% 50%");if(v=="top")return {w/2,0};auto a=split(v);return {a.empty()?w/2:len(a[0],w),a.size()<2?h/2:len(a[1],h)};}
struct Plane {
    std::array<double,9> m{1,0,0,0,1,0,0,0,1};
    Plane operator*(const Plane& b)const{Plane out;out.m.fill(0);for(unsigned y=0;y<3;++y)for(unsigned x=0;x<3;++x)for(unsigned k=0;k<3;++k)out.m[y*3+x]+=m[y*3+k]*b.m[k*3+x];return out;}
    std::array<double,2> point(double x,double y)const{double z=m[6]*x+m[7]*y+m[8];if(std::abs(z)<1e-10)return {1e10,1e10};return {(m[0]*x+m[1]*y+m[2])/z,(m[3]*x+m[4]*y+m[5])/z};}
    bool inverse(Plane& out)const{auto& a=m;const double det=a[0]*(a[4]*a[8]-a[5]*a[7])-a[1]*(a[3]*a[8]-a[5]*a[6])+a[2]*(a[3]*a[7]-a[4]*a[6]);if(std::abs(det)<1e-10)return false;out.m={(a[4]*a[8]-a[5]*a[7])/det,(a[2]*a[7]-a[1]*a[8])/det,(a[1]*a[5]-a[2]*a[4])/det,(a[5]*a[6]-a[3]*a[8])/det,(a[0]*a[8]-a[2]*a[6])/det,(a[2]*a[3]-a[0]*a[5])/det,(a[3]*a[7]-a[4]*a[6])/det,(a[1]*a[6]-a[0]*a[7])/det,(a[0]*a[4]-a[1]*a[3])/det};return true;}
    static Plane translation(double x,double y){return {{1,0,x,0,1,y,0,0,1}};}
};
Plane plane(const ProbeSceneNode& n){auto o=origin(n.style,n.box[2],n.box[3]);auto t=get(n.style,"transform","none");Plane local;
    if(t.find("perspective(")!=t.npos){static const std::regex grammar(R"(perspective\(\s*([\d.]+)px\s*\)\s+rotateY\(\s*([-\d.]+)(deg)?\s*\))");std::smatch match;if(!std::regex_match(t,match,grammar))fail("projective plane grammar");const double d=num(match[1]),a=num(match[2])*(match[3].matched?pi/180:1);if(d<=0)fail("perspective distance");local.m={std::cos(a),0,0,0,1,0,std::sin(a)/d,0,1};}
    else{auto a=transform(t,n.box[2],n.box[3],false);local.m={a._11,a._21,a._31,a._12,a._22,a._32,0,0,1};}
    return Plane::translation(o[0]+n.box[0],o[1]+n.box[1])*local*Plane::translation(-o[0],-o[1]);
}
Plane affine_plane(D2D1::Matrix3x2F m){return {{m._11,m._21,m._31,m._12,m._22,m._32,0,0,1}};}
D2D1::Matrix3x2F svg_viewport(const ProbeSceneNode& n){auto i=n.attributes.find("viewBox");if(i==n.attributes.end())return D2D1::Matrix3x2F::Identity();Scanner s{i->second};const auto x=s.number(),y=s.number(),w=s.number(),h=s.number();if(!s.end()||w<=0||h<=0)fail("viewBox dimensions");float sx=n.box[2]/w,sy=n.box[3]/h,dx{},dy{};auto aspect=n.attributes.contains("preserveAspectRatio")?n.attributes.at("preserveAspectRatio"):"xMidYMid meet";auto a=split(aspect);if(a.empty())fail("preserveAspectRatio");if(a[0]!="none"){if(a[0].size()!=8 || a[0][0]!='x' || a[0][4]!='Y' || a.size()>2)fail("preserveAspectRatio alignment");auto mode=a.size()>1?a[1]:"meet";if(mode!="meet"&&mode!="slice")fail("preserveAspectRatio mode");sx=sy=mode=="slice"?std::max(sx,sy):std::min(sx,sy);auto align=[](std::string v){if(v=="Min")return 0.f;if(v=="Mid")return .5f;if(v=="Max")return 1.f;fail("preserveAspectRatio alignment");};dx=(n.box[2]-w*sx)*align(a[0].substr(1,3));dy=(n.box[3]-h*sy)*align(a[0].substr(5,3));}return {sx,0,0,sy,dx-x*sx,dy-y*sy};}
}
struct SceneRasterProbe::Impl {
    const std::thread::id owner=std::this_thread::get_id();
    void require_owner()const{if(std::this_thread::get_id()!=owner)throw std::logic_error("CE_SCENE_RASTER_OWNER");}
    ComPtr<ID2D1Factory> factory;ComPtr<IWICImagingFactory> wic;ComPtr<IDWriteFactory> write;
    std::wstring system_font=L"Segoe UI";ComPtr<IWICBitmap> backing;unsigned backing_width{},backing_height{};
    std::map<std::tuple<std::string,std::string,float,int,float,float>,ComPtr<IDWriteTextLayout>> text_cache;
    std::size_t text_cache_bytes{};
    std::map<std::tuple<float,float,std::array<float,4>>,ComPtr<ID2D1PathGeometry>> round_cache;
    std::map<std::string,ComPtr<ID2D1PathGeometry>> path_cache;std::size_t path_cache_bytes{};
    struct Shape {ComPtr<ID2D1Geometry> geometry;std::size_t used{};};
    std::map<std::string,Shape> shapes;std::size_t shape_bytes{},shape_clock{};
    std::map<std::string,ComPtr<ID2D1StrokeStyle>> strokes;std::size_t stroke_bytes{};
    std::map<std::tuple<std::string,std::string,std::string,float,float,float>,ComPtr<ID2D1Bitmap>> gradient_cache;std::size_t gradient_cache_bytes{};
    struct ShadowBitmap {ComPtr<ID2D1Bitmap> bitmap;D2D1_RECT_F rect;std::size_t bytes;};
    std::map<std::string,ShadowBitmap> shadows;std::size_t shadow_bytes{};
    ComPtr<ID2D1RenderTarget> target;ComPtr<ID2D1DeviceContext> device_context;ComPtr<ID2D1SolidColorBrush> brush;float scale=1;
    Impl(){NONCLIENTMETRICSW metrics{};metrics.cbSize=sizeof(metrics);if(SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0))system_font=metrics.lfMessageFont.lfFaceName;check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf()));check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)));check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(write.GetAddressOf())));}
    ComPtr<IDWriteTextLayout> text_layout(std::string_view text,const Style& s,float width=100000){
        auto family=get(s,"font-family","Segoe UI");if(family.find("Consolas")!=family.npos || family=="monospace")family="Consolas";else if(family=="sans-serif")family="Arial";else if(family.find("system-ui")!=family.npos || family.find("ui-sans-serif")!=family.npos)family="Segoe UI";else if(family.starts_with('"')){auto end=family.find('"',1);family=family.substr(1,end-1);}else if(family.find(',')!=family.npos)family=trim(family.substr(0,family.find(',')));
        const auto font_size=len(get(s,"font-size","16px"));auto weight=get(s,"font-weight","400");const auto bold=weight=="bold"?700:weight=="normal"?400:num(weight);
        float spacing=len(get(s,"letter-spacing","0"),font_size);auto key=std::tuple{std::string(text),family,font_size,int(bold),std::max(1.f,width),spacing};
        if(auto i=text_cache.find(key);i!=text_cache.end())return i->second;
        ComPtr<IDWriteTextFormat> format;check(write->CreateTextFormat((family=="Segoe UI"?system_font:utf16(family)).c_str(),nullptr,DWRITE_FONT_WEIGHT(int(bold)),DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,font_size,L"en-us",&format));check(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
        auto value=utf16(text);ComPtr<IDWriteTextLayout> layout;check(write->CreateTextLayout(value.c_str(),UINT(value.size()),format.Get(),std::max(1.f,width),100000,&layout));ComPtr<IDWriteTextLayout1> kern;if(SUCCEEDED(layout.As(&kern)))check(kern->SetPairKerning(TRUE,{0,UINT(value.size())}));
        if(spacing!=0 && kern)check(kern->SetCharacterSpacing(0,spacing,0,{0,UINT(value.size())}));
        if(text_cache.size()>=256 || text_cache_bytes+text.size()>2*1024*1024){text_cache.clear();text_cache_bytes=0;}text_cache_bytes+=text.size();text_cache.emplace(std::move(key),layout);return layout;
    }
    ProbeSceneTextMetrics measure(std::string_view text,const Style& s){auto layout=text_layout(text,s);DWRITE_TEXT_METRICS m{};check(layout->GetMetrics(&m));DWRITE_LINE_METRICS line{};UINT count{};check(layout->GetLineMetrics(&line,1,&count));float size=len(get(s,"font-size","16px")),height=m.height;auto lh=get(s,"line-height","1.5");if(lh!="normal")height=lh.ends_with("px")?len(lh):num(lh)*size;return {m.widthIncludingTrailingWhitespace,height,line.baseline+(height-m.height)/2};}
    ComPtr<ID2D1PathGeometry> path(std::string value,bool evenodd=false){
        if(value.size()>262144)fail("SVG path byte bound");if(auto i=path_cache.find((evenodd?"evenodd:":"winding:")+value);i!=path_cache.end())return i->second;auto key=(evenodd?"evenodd:":"winding:")+value;ComPtr<ID2D1PathGeometry> geometry;check(factory->CreatePathGeometry(&geometry));ComPtr<ID2D1GeometrySink> sink;check(geometry->Open(&sink));sink->SetFillMode(evenodd?D2D1_FILL_MODE_ALTERNATE:D2D1_FILL_MODE_WINDING);Scanner s{std::move(value)};char command{};D2D1_POINT_2F point{},start{};bool open{};unsigned segments{};
        char previous{};D2D1_POINT_2F control{};
        while(!s.end()){
            if(++segments>65536)fail("path segment bound");if(!s.has_number())command=s.value[s.p++];const bool relative=std::islower(static_cast<unsigned char>(command));const char c=char(std::toupper(static_cast<unsigned char>(command)));
            auto endpoint=[&]{float x=s.number(),y=s.number();return D2D1::Point2F(x+(relative?point.x:0),y+(relative?point.y:0));};
            if(c=='M'){auto p=endpoint();if(open)sink->EndFigure(D2D1_FIGURE_END_OPEN);sink->BeginFigure(p,D2D1_FIGURE_BEGIN_FILLED);point=start=p;open=true;command=relative?'l':'L';}
            else if(c=='Z'){if(open)sink->EndFigure(D2D1_FIGURE_END_CLOSED);open=false;point=start;command=0;}
            else {if(!open){sink->BeginFigure(point,D2D1_FIGURE_BEGIN_FILLED);start=point;open=true;}
                if(c=='L'){point=endpoint();sink->AddLine(point);}
                else if(c=='H'){point.x=s.number()+(relative?point.x:0);sink->AddLine(point);}
                else if(c=='V'){point.y=s.number()+(relative?point.y:0);sink->AddLine(point);}
                else if(c=='C'){auto p1=endpoint(),p2=endpoint(),p3=endpoint();sink->AddBezier(D2D1::BezierSegment(p1,p2,p3));control=p2;point=p3;}
                else if(c=='S'){auto p1=previous=='C'||previous=='S'?D2D1::Point2F(2*point.x-control.x,2*point.y-control.y):point;auto p2=endpoint(),p3=endpoint();sink->AddBezier(D2D1::BezierSegment(p1,p2,p3));control=p2;point=p3;}
                else if(c=='Q'){auto p1=endpoint(),p2=endpoint();sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(p1,p2));control=p1;point=p2;}
                else if(c=='T'){auto p1=previous=='Q'||previous=='T'?D2D1::Point2F(2*point.x-control.x,2*point.y-control.y):point;auto p2=endpoint();sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(p1,p2));control=p1;point=p2;}
                else if(c=='A'){auto rx=std::abs(s.number()),ry=std::abs(s.number()),angle=s.number(),large=s.number(),sweep=s.number();auto p=endpoint();if((large!=0 && large!=1) || (sweep!=0 && sweep!=1))fail("arc flags");if(p.x!=point.x || p.y!=point.y){if(rx==0 || ry==0)sink->AddLine(p);else sink->AddArc(D2D1::ArcSegment(p,D2D1::SizeF(rx,ry),angle,sweep?D2D1_SWEEP_DIRECTION_CLOCKWISE:D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,large?D2D1_ARC_SIZE_LARGE:D2D1_ARC_SIZE_SMALL));}point=p;}
                else fail("unsupported SVG path command "+std::string(1,c));
            }previous=c;
        }if(open)sink->EndFigure(D2D1_FIGURE_END_OPEN);check(sink->Close());if(path_cache.size()>=128 || path_cache_bytes+key.size()>2*1024*1024){path_cache.clear();path_cache_bytes=0;}path_cache_bytes+=key.size();path_cache.emplace(std::move(key),geometry);return geometry;
    }
    ComPtr<ID2D1PathGeometry> round_box(float w,float h,std::array<float,4> r){
        for(auto& v:r)v=std::max(0.f,v);float factor=1;for(auto pair:std::vector<std::pair<float,float>>{{w,r[0]+r[1]},{w,r[2]+r[3]},{h,r[0]+r[3]},{h,r[1]+r[2]}})if(pair.second>0)factor=std::min(factor,pair.first/pair.second);for(auto& v:r)v*=factor;
        auto key=std::tuple{w,h,r};if(auto i=round_cache.find(key);i!=round_cache.end())return i->second;
        ComPtr<ID2D1PathGeometry> g;check(factory->CreatePathGeometry(&g));ComPtr<ID2D1GeometrySink> s;check(g->Open(&s));s->BeginFigure({r[0],0},D2D1_FIGURE_BEGIN_FILLED);
        auto arc=[&](D2D1_POINT_2F end,float radius){if(radius)s->AddArc(D2D1::ArcSegment(end,{radius,radius},0,D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));else s->AddLine(end);};
        s->AddLine({w-r[1],0});arc({w,r[1]},r[1]);s->AddLine({w,h-r[2]});arc({w-r[2],h},r[2]);s->AddLine({r[3],h});arc({0,h-r[3]},r[3]);s->AddLine({0,r[0]});arc({r[0],0},r[0]);s->EndFigure(D2D1_FIGURE_END_CLOSED);check(s->Close());if(round_cache.size()>=256)round_cache.clear();round_cache.emplace(key,g);return g;
    }
    std::map<std::array<float,7>,ComPtr<ID2D1PathGeometry>> border_wedges;
    // Factory-owned geometry, with no scene/node references. Widen the same
    // stroke as the reference, then intersect its masks once instead of
    // rebuilding isolation layers on every animated frame.
    std::map<std::array<float,12>,ComPtr<ID2D1PathGeometry>> border_fills;
    ComPtr<ID2D1PathGeometry> border_fill(const ProbeSceneNode& n,const std::array<float,4>& radii,ID2D1Geometry* shape,float width,unsigned side){
        std::array<float,12> key{n.box[2],n.box[3],radii[0],radii[1],radii[2],radii[3],width,float(side)};unsigned k=8;
        for(const auto* edge:{"top","right","bottom","left"})key[k++]=len(get(n.style,std::string("border-")+edge+"-width","0"));
        if(auto i=border_fills.find(key);i!=border_fills.end()){++work.border_geometry_reuses;return i->second;}
        ComPtr<ID2D1PathGeometry> widened,ring,result;ComPtr<ID2D1GeometrySink> sink;
        check(factory->CreatePathGeometry(&widened));check(widened->Open(&sink));
        check(shape->Widen(width*2,nullptr,nullptr,.01f,sink.Get()));check(sink->Close());sink.Reset();
        check(factory->CreatePathGeometry(&ring));check(ring->Open(&sink));
        check(shape->CombineWithGeometry(widened.Get(),D2D1_COMBINE_MODE_INTERSECT,nullptr,.01f,sink.Get()));check(sink->Close());sink.Reset();
        if(side<4){auto wedge=border_wedge(n,side);check(factory->CreatePathGeometry(&result));check(result->Open(&sink));
            check(ring->CombineWithGeometry(wedge.Get(),D2D1_COMBINE_MODE_INTERSECT,nullptr,.01f,sink.Get()));check(sink->Close());}
        else result=ring;
        if(border_fills.size()>=256)border_fills.clear();border_fills.emplace(key,result);++work.border_geometry_builds;return result;
    }
    ComPtr<ID2D1PathGeometry> border_wedge(const ProbeSceneNode& n,unsigned index){
        float w=n.box[2],h=n.box[3];std::array<float,7> key{w,h,len(get(n.style,"border-top-width","0")),len(get(n.style,"border-right-width","0")),len(get(n.style,"border-bottom-width","0")),len(get(n.style,"border-left-width","0")),float(index)};
        if(auto i=border_wedges.find(key);i!=border_wedges.end())return i->second;
        ComPtr<ID2D1PathGeometry> wedge;check(factory->CreatePathGeometry(&wedge));ComPtr<ID2D1GeometrySink> sink;check(wedge->Open(&sink));
        const D2D1_POINT_2F corners[4]={{0,0},{w,0},{w,h},{0,h}},inner[4]={{key[5],key[2]},{w-key[3],key[2]},{w-key[3],h-key[4]},{key[5],h-key[4]}};
        bool equal=true;for(unsigned i=3;i<=5;++i)equal=equal && std::abs(key[i]-key[2])<1e-5;
        sink->BeginFigure(equal?D2D1::Point2F(w/2,h/2):inner[index],D2D1_FIGURE_BEGIN_FILLED);sink->AddLine(corners[index]);sink->AddLine(corners[(index+1)%4]);if(!equal)sink->AddLine(inner[(index+1)%4]);sink->EndFigure(D2D1_FIGURE_END_CLOSED);check(sink->Close());if(border_wedges.size()>=256)border_wedges.clear();border_wedges.emplace(key,wedge);return wedge;
    }
    std::vector<ComPtr<ID2D1Layer>> layers;std::size_t layer_cursor{};
    ComPtr<ID2D1Layer> layer(){if(layer_cursor==layers.size()){if(layers.size()>=512)fail("layer cache capacity");ComPtr<ID2D1Layer> value;check(target->CreateLayer(&value));layers.push_back(value);}return layers[layer_cursor++];}
    struct ProjectiveContent {ProbeSceneNode node;float scale{};ProbeSceneBitmap image;ComPtr<ID2D1Bitmap> bitmap;};
    std::map<std::array<std::uint64_t,3>,ProjectiveContent> projected;std::size_t projected_bytes{};
    bool gpu_target{};SceneRasterProbe::Work work;
    // Match the RGBA8 premultiplied brush precision of the software oracle.
    void solid(D2D1_COLOR_F c){if(gpu_target && c.a>0 && c.a<1){auto alpha=std::round(c.a*255)/255;auto component=[&](float v){return alpha?std::round(v*c.a*255)/(255*alpha):0;};c={component(c.r),component(c.g),component(c.b),alpha};}brush->SetColor(c);}
    ComPtr<ID2D1Geometry> svg_shape(const ProbeSceneNode& n){const auto& a=n.attributes;
        if(n.tag=="text"||n.tag=="g"||n.tag=="svg"||n.tag=="defs"||n.tag=="clipPath")return {};
        std::string key=n.tag;auto add=[&](std::string_view v){key+=std::to_string(v.size())+":";key+=v;};add(get(n.style,"fill-rule"));for(const auto* k:{"x","y","width","height","rx","ry","cx","cy","r","d","points","x1","y1","x2","y2"})if(a.contains(k)){add(k);add(a.at(k));}
        if(auto it=shapes.find(key);it!=shapes.end()){it->second.used=++shape_clock;return it->second.geometry;}
        auto v=[&](const char* k,float def=0){auto i=a.find(k);return i==a.end()?def:len(i->second);};ComPtr<ID2D1Geometry> g;
        if(n.tag=="rect"){ComPtr<ID2D1RoundedRectangleGeometry> r;float rx=v("rx"),ry=v("ry",rx);check(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect({v("x"),v("y"),v("x")+v("width"),v("y")+v("height")},rx,ry),&r));g=r;}
        else if(n.tag=="circle"){ComPtr<ID2D1EllipseGeometry> e;check(factory->CreateEllipseGeometry(D2D1::Ellipse({v("cx"),v("cy")},v("r"),v("r")),&e));g=e;}
        else if(n.tag=="ellipse"){ComPtr<ID2D1EllipseGeometry> e;check(factory->CreateEllipseGeometry(D2D1::Ellipse({v("cx"),v("cy")},v("rx"),v("ry")),&e));g=e;}
        else if(n.tag=="path"){if(a.contains("d"))g=path(a.at("d"),get(n.style,"fill-rule")=="evenodd");}
        else if(n.tag=="polygon" || n.tag=="polyline"){std::ostringstream p;Scanner s{a.contains("points")?a.at("points"):""};bool first=true;while(!s.end()){float x=s.number(),y=s.number();p<<(first?'M':'L')<<x<<','<<y;first=false;}if(n.tag=="polygon")p<<'Z';g=path(p.str(),get(n.style,"fill-rule")=="evenodd");}
        else if(n.tag=="line"){std::ostringstream p;p<<'M'<<v("x1")<<','<<v("y1")<<'L'<<v("x2")<<','<<v("y2");g=path(p.str(),get(n.style,"fill-rule")=="evenodd");}
        else if(n.tag!="text" && n.tag!="g" && n.tag!="svg" && n.tag!="defs" && n.tag!="clipPath")fail("SVG element "+n.tag);
        if(g && key.size()<=4*1024*1024){while(!shapes.empty() && (shapes.size()>=1024 || shape_bytes+key.size()>4*1024*1024)){auto victim=std::min_element(shapes.begin(),shapes.end(),[](const auto& x,const auto& y){return x.second.used<y.second.used;});shape_bytes-=victim->first.size();shapes.erase(victim);}shape_bytes+=key.size();shapes.emplace(std::move(key),Shape{g,++shape_clock});}return g;
    }
    ComPtr<ID2D1StrokeStyle> svg_stroke(const ProbeSceneNode& n){std::string key;for(const auto* k:{"stroke-linecap","stroke-linejoin","stroke-width","stroke-dasharray","stroke-dashoffset"}){const auto v=get(n.style,k);key+=std::to_string(v.size())+":"+v;}if(auto it=strokes.find(key);it!=strokes.end())return it->second;auto result=svg_stroke_uncached(n);if(key.size()<=2*1024*1024){if(strokes.size()>=128 || stroke_bytes+key.size()>2*1024*1024){strokes.clear();stroke_bytes=0;}stroke_bytes+=key.size();strokes.emplace(std::move(key),result);}return result;}
    ComPtr<ID2D1StrokeStyle> svg_stroke_uncached(const ProbeSceneNode& n){D2D1_STROKE_STYLE_PROPERTIES p=D2D1::StrokeStyleProperties();auto cap=get(n.style,"stroke-linecap","butt"),join=get(n.style,"stroke-linejoin","miter");if(cap!="butt"&&cap!="round"&&cap!="square"&&!cap.empty())fail("stroke linecap");if(join!="miter"&&join!="round"&&join!="bevel"&&!join.empty())fail("stroke linejoin");p.startCap=p.endCap=p.dashCap=cap=="round"?D2D1_CAP_STYLE_ROUND:cap=="square"?D2D1_CAP_STYLE_SQUARE:D2D1_CAP_STYLE_FLAT;p.lineJoin=join=="round"?D2D1_LINE_JOIN_ROUND:join=="bevel"?D2D1_LINE_JOIN_BEVEL:D2D1_LINE_JOIN_MITER;const auto width=len(get(n.style,"stroke-width","1"));auto d=get(n.style,"stroke-dasharray","none");std::vector<float> dash;if(d!="none"&&!d.empty()){Scanner s{d};while(!s.end()){auto v=s.number();if(v<0||dash.size()>=64)fail("stroke dash bound");dash.push_back(v);}if(dash.size()%2){auto copy=dash;dash.insert(dash.end(),copy.begin(),copy.end());}if(std::all_of(dash.begin(),dash.end(),[](float v){return v==0;}))dash.clear();for(auto& v:dash)v/=width>0?width:1;}if(!dash.empty())p.dashStyle=D2D1_DASH_STYLE_CUSTOM;p.dashOffset=len(get(n.style,"stroke-dashoffset","0"))/(width>0?width:1);ComPtr<ID2D1StrokeStyle> style;check(factory->CreateStrokeStyle(p,dash.empty()?nullptr:dash.data(),UINT32(dash.size()),&style));return style;}
    ComPtr<ID2D1Geometry> svg_clip(const ProbeSceneNode& n,const ProbeSceneNode& root){auto v=n.attributes.contains("clip-path")?n.attributes.at("clip-path"):"";if(v.empty()||v=="none")return {};if(!v.starts_with("url(#")||!v.ends_with(')'))fail("local clip-path required");const auto id=v.substr(5,v.size()-6);auto find=[&](auto&& self,const auto& c)->const ProbeSceneNode*{if(c.attributes.contains("id")&&c.attributes.at("id")==id)return &c;for(const auto& child:c.children)if(auto p=self(self,child))return p;return nullptr;};auto* definition=find(find,root);if(!definition||definition->tag!="clipPath"||definition->children.size()!=1||definition->children[0].tag!="rect"||get(definition->attributes,"clipPathUnits","userSpaceOnUse")!="userSpaceOnUse")fail("clipPath requires one user-space rect");auto g=svg_shape(definition->children[0]);return g;}
    void svg(const ProbeSceneNode& n,const ProbeSceneNode& root){if(get(n.style,"display")=="none" || n.tag=="defs" || n.tag=="clipPath")return;const auto old=matrix();const auto t=n.attributes.contains("transform")?n.attributes.at("transform"):get(n.style,"transform");target->SetTransform(transform(t,0,0,true)*old);ComPtr<ID2D1Layer> group,clip;const auto opacity=len(get(n.style,"opacity","1"));if(opacity<1){group=layer();target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),std::clamp(opacity,0.f,1.f)),group.Get());}auto mask=svg_clip(n,root);if(mask){clip=layer();target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),mask.Get()),clip.Get());}auto g=svg_shape(n);if(n.tag=="text")draw_text(n,true);
        if(g){auto fill=get(n.style,"fill","#000000"),stroke=get(n.style,"stroke","none");if(fill!="none" && n.tag!="line"){auto c=color(fill,n.style);c.a*=std::clamp(len(get(n.style,"fill-opacity","1")),0.f,1.f);solid(c);target->FillGeometry(g.Get(),brush.Get());}if(stroke!="none"){auto c=color(stroke,n.style);c.a*=std::clamp(len(get(n.style,"stroke-opacity","1")),0.f,1.f);solid(c);auto style=svg_stroke(n);target->DrawGeometry(g.Get(),brush.Get(),len(get(n.style,"stroke-width","1")),style.Get());}}
        for(const auto& c:n.children)svg(c,root);if(clip)target->PopLayer();if(group)target->PopLayer();target->SetTransform(old);
    }
    D2D1::Matrix3x2F matrix(){D2D1::Matrix3x2F m;target->GetTransform(&m);return m;}
    struct ColorText {ProbeSceneNode key;float scale{};ComPtr<ID2D1Bitmap> bitmap;std::size_t bytes{};};
    std::map<std::array<std::uint64_t,3>,ColorText> color_text;std::size_t color_text_bytes{};
    bool color_text_bitmap(const ProbeSceneNode& n){
        // System color glyphs otherwise regenerate their SVG/bitmap at every
        // animated scale. Keep bounded device-ppem text images, like a glyph
        // cache; geometry/opacity/transform remain live and are applied outside.
        if(!gpu_target || std::none_of(n.text.begin(),n.text.end(),[](char c){return (static_cast<unsigned char>(c)&0xf8)==0xf0;}))return false;
        auto content=n;content.box[0]=content.box[1]=0;content.children.clear();content.style.erase("transform");content.style["opacity"]="1";content.style["background-color"]="transparent";content.style["background-image"]="none";
        for(const auto* side:{"top","right","bottom","left"})content.style[std::string("border-")+side+"-color"]="transparent";
        const std::array<std::uint64_t,3> id{n.identity.view_epoch,n.identity.slot,n.identity.generation};auto it=color_text.find(id);
        if(it==color_text.end() || it->second.key!=content || it->second.scale!=scale){
            if(std::ceil(n.box[2]*scale)*std::ceil(n.box[3]*scale)>2*1024*1024)return false;
            ProbeSceneFrame frame;frame.root=content;SceneRasterProbe software;auto image=software.raster(frame,scale);
            if(color_text.size()>=32 || color_text_bytes+image.rgba.size()>8*1024*1024){color_text.clear();color_text_bytes=0;}it=color_text.find(id);if(it!=color_text.end()){color_text_bytes-=it->second.bytes;color_text.erase(it);}
            auto bytes=image.rgba;for(std::size_t p=0;p<bytes.size();p+=4){auto alpha=bytes[p+3];std::swap(bytes[p],bytes[p+2]);for(unsigned c=0;c<3;++c)bytes[p+c]=std::uint8_t((unsigned(bytes[p+c])*alpha+127)/255);}
            ComPtr<ID2D1Bitmap> bitmap;check(target->CreateBitmap({image.width,image.height},bytes.data(),image.width*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&bitmap));color_text_bytes+=bytes.size();it=color_text.emplace(id,ColorText{std::move(content),scale,bitmap,bytes.size()}).first;
        }
        target->DrawBitmap(it->second.bitmap.Get(),{0,0,n.box[2],n.box[3]});return true;
    }
    void draw_text(const ProbeSceneNode& n,bool svg_text=false){
        if(n.text.empty())return;if(!svg_text && color_text_bitmap(n))return;auto layout=text_layout(n.text,n.style,n.box[2]);DWRITE_TEXT_METRICS m{};check(layout->GetMetrics(&m));DWRITE_LINE_METRICS line{};UINT count{};check(layout->GetLineMetrics(&line,1,&count));float size=len(get(n.style,"font-size","16px")),x{},y{};
        if(svg_text){x=n.attributes.contains("x")?len(n.attributes.at("x")):0;y=n.attributes.contains("y")?len(n.attributes.at("y")):0;
            if(n.attributes.contains("dy"))y+=len(n.attributes.at("dy"),size);
            auto anchor=get(n.style,"text-anchor","start");if(anchor=="middle")x-=m.width/2;else if(anchor=="end")x-=m.width;
            y-=get(n.style,"dominant-baseline")=="central"?m.height/2:line.baseline;
        }else{const auto align=get(n.style,"text-align","left");if(align=="center")x=(n.box[2]-m.width)/2;else if(align=="right")x=n.box[2]-m.width;
            auto lh=get(n.style,"line-height","1.5");float height=lh=="normal"?m.height:lh.ends_with("px")?len(lh):num(lh)*size;y=(height-m.height)/2;if(get(n.style,"display")=="flex"){if(get(n.style,"justify-content")=="center")x=(n.box[2]-m.width)/2;if(get(n.style,"align-items")=="center")y=(n.box[3]-m.height)/2;}x+=len(get(n.style,"padding-left","0"))+len(get(n.style,"border-left-width","0"));y+=len(get(n.style,"padding-top","0"))+len(get(n.style,"border-top-width","0"));}
        solid(svg_text?color(get(n.style,"fill","#000000"),n.style):paint_color(n,3,get(n.style,"color","#000000")));target->DrawTextLayout({x,y},layout.Get(),brush.Get(),D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }
    struct Stop {float offset{};D2D1_COLOR_F color{};bool explicit_offset{};};
    std::vector<Stop> stops(std::vector<std::string> values,float reference=1){std::vector<Stop> out;for(auto s:values){auto a=split(s);if(a.empty())fail("gradient stop");auto c=color(a[0]);if(a.size()==1)out.push_back({0,c,false});else for(std::size_t i=1;i<a.size();++i)out.push_back({a[i].ends_with('%')?len(a[i]):len(a[i])/reference,c,true});}if(out.size()<2)fail("gradient stop count");if(!out.front().explicit_offset)out.front().offset=0;if(!out.back().explicit_offset)out.back().offset=1;out.front().explicit_offset=out.back().explicit_offset=true;float previous=out.front().offset;for(auto& s:out)if(s.explicit_offset){s.offset=std::max(s.offset,previous);previous=s.offset;}
        for(std::size_t i=1;i<out.size()-1;){if(out[i].explicit_offset){++i;continue;}auto j=i;while(j<out.size() && !out[j].explicit_offset)++j;for(auto k=i;k<j;++k)out[k].offset=out[i-1].offset+(out[j].offset-out[i-1].offset)*float(k-i+1)/float(j-i+1);i=j;}return out;}
    static D2D1_COLOR_F sampled(const std::vector<Stop>& s,float t){if(t<=s.front().offset)return s.front().color;if(t>=s.back().offset)return s.back().color;std::size_t i=1;while(i<s.size() && s[i].offset<=t)++i;const auto a=s[i-1],b=s[i];float f=(t-a.offset)/(b.offset-a.offset),alpha=a.color.a+(b.color.a-a.color.a)*f;auto component=[&](float x,float y){return alpha?((1-f)*x*a.color.a+f*y*b.color.a)/alpha:0;};return {component(a.color.r,b.color.r),component(a.color.g,b.color.g),component(a.color.b,b.color.b),alpha};}
    void shadow(const ProbeSceneNode& n,const std::array<float,4>& corners){auto value=get(n.style,"box-shadow","none");if(value.empty()||value=="none")return;auto items=split(value,',');if(items.size()>8)fail("shadow layer bound");
        const auto w=n.box[2],h=n.box[3];auto distance=[](float x,float y,float width,float height,const std::array<float,4>& radii){const auto r=std::min(std::min(width,height)/2,radii[y<height/2?(x<width/2?0:1):(x<width/2?3:2)]);const auto qx=std::abs(x-width/2)-(width/2-r),qy=std::abs(y-height/2)-(height/2-r);return std::hypot(std::max(qx,0.f),std::max(qy,0.f))+std::min(std::max(qx,qy),0.f)-r;};
        for(auto it=items.rbegin();it!=items.rend();++it){auto parts=split(*it);if(parts.size()<3||parts.size()>5||parts[0]=="inset")fail("outer shadow lengths and color required");auto ctext=parts.back();D2D1_COLOR_F c{};if(ctext=="#0000")continue;if(ctext.starts_with("rgb(")&&ctext.find('/')!=ctext.npos){auto v=ctext.substr(4,ctext.size()-5);std::replace(v.begin(),v.end(),'/',' ');std::istringstream parser(v);float r{},g{},b{},a{};if(!(parser>>r>>g>>b>>a))fail("shadow color");c={r/255,g/255,b/255,a};}else c=color(ctext,n.style);if(c.a==0)continue;const auto x=len(parts[0]),y=len(parts[1]),blur=parts.size()>3?len(parts[2]):0,spread=parts.size()>4?len(parts[3]):0;if(blur<0||blur>64||std::abs(spread)>64)fail("shadow blur/spread bound");if(w+2*spread<=0||h+2*spread<=0)continue;
            std::ostringstream key;key<<*it<<'|'<<w<<'|'<<h<<'|'<<scale;for(auto r:corners)key<<'|'<<r;auto cached=shadows.find(key.str());if(cached==shadows.end()){const auto sigma=blur*scale/2,guard=std::ceil(3*sigma)+2;const float left=x-spread-guard/scale,top=y-spread-guard/scale;const unsigned iw=unsigned(std::ceil((w+2*spread)*scale+2*guard)),ih=unsigned(std::ceil((h+2*spread)*scale+2*guard));const auto count=std::size_t(iw)*ih;if(!iw||!ih||count>1048576)fail("shadow bitmap bound");std::vector<float> mask(count),temporary(count),filtered(count);auto radii=corners;for(auto& r:radii)r=std::max(0.f,r+spread);for(unsigned row=0;row<ih;++row)for(unsigned col=0;col<iw;++col){const float px=(col+.5f)/scale-guard/scale,py=(row+.5f)/scale-guard/scale;mask[std::size_t(row)*iw+col]=std::clamp(.5f-distance(px,py,w+2*spread,h+2*spread,radii)*scale,0.f,1.f);}
                if(sigma>0){const int radius=int(std::ceil(3*sigma));std::vector<float> kernel(2*radius+1);float total{};for(int k=-radius;k<=radius;++k)total+=kernel[k+radius]=std::exp(-float(k*k)/(2*sigma*sigma));for(auto& v:kernel)v/=total;for(unsigned row=0;row<ih;++row)for(unsigned col=0;col<iw;++col){float v{};for(int k=-radius;k<=radius;++k)if(int(col)+k>=0&&int(col)+k<int(iw))v+=mask[std::size_t(row)*iw+col+k]*kernel[k+radius];temporary[std::size_t(row)*iw+col]=v;}for(unsigned row=0;row<ih;++row)for(unsigned col=0;col<iw;++col){float v{};for(int k=-radius;k<=radius;++k)if(int(row)+k>=0&&int(row)+k<int(ih))v+=temporary[std::size_t(int(row)+k)*iw+col]*kernel[k+radius];filtered[std::size_t(row)*iw+col]=v;}}else filtered=mask;
                std::vector<std::uint8_t> pixels(count*4);for(unsigned row=0;row<ih;++row)for(unsigned col=0;col<iw;++col){const auto index=std::size_t(row)*iw+col;const auto outside=std::clamp(.5f+distance(left+(col+.5f)/scale,top+(row+.5f)/scale,w,h,corners)*scale,0.f,1.f);const auto alpha=filtered[index]*outside*c.a;auto p=pixels.data()+index*4;p[0]=std::uint8_t(std::clamp(c.b*alpha*255+.5f,0.f,255.f));p[1]=std::uint8_t(std::clamp(c.g*alpha*255+.5f,0.f,255.f));p[2]=std::uint8_t(std::clamp(c.r*alpha*255+.5f,0.f,255.f));p[3]=std::uint8_t(std::clamp(alpha*255+.5f,0.f,255.f));}ShadowBitmap entry;entry.rect={left,top,left+iw/scale,top+ih/scale};entry.bytes=pixels.size();check(target->CreateBitmap({iw,ih},pixels.data(),iw*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&entry.bitmap));if(shadows.size()>=32||shadow_bytes+entry.bytes>16*1024*1024){shadows.clear();shadow_bytes=0;}shadow_bytes+=entry.bytes;cached=shadows.emplace(key.str(),std::move(entry)).first;}
            target->DrawBitmap(cached->second.bitmap.Get(),cached->second.rect,1,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }
    void gradient(const ProbeSceneNode& n,ID2D1Geometry* shape){auto image=get(n.style,"background-image","none");if(image=="none")return;auto b=image.find('('),e=image.rfind(')');if(b==image.npos || e==image.npos)fail("gradient syntax");auto kind=image.substr(0,b);auto args=split(image.substr(b+1,e-b-1),',');float angle=180;bool linear=kind=="linear-gradient",radial=kind=="radial-gradient",conic=kind=="conic-gradient";
        if(!linear && !radial && !conic)fail("background image "+kind);
        if(linear && args.front().ends_with("deg")){angle=num(args.front().substr(0,args.front().size()-3));args.erase(args.begin());}
        if(radial && args.front()=="circle")args.erase(args.begin());if(conic && args.front().starts_with("from ")){auto a=args.front().substr(5);angle=num(a.substr(0,a.size()-3));args.erase(args.begin());}else if(conic)angle=0;
        float w=n.box[2],h=n.box[3],bw=w,bh=h;auto size=split(get(n.style,"background-size","100% 100%"));if(size.size()==2){bw=len(size[0],w);bh=len(size[1],h);}if(bw<=0||bh<=0)fail("positive background tile required");const auto stop_reference=radial?std::hypot(bw/2,bh/2):linear?std::abs(bw*std::sin(angle*pi/180))+std::abs(bh*std::cos(angle*pi/180)):1.f;auto s=stops(args,stop_reference);auto position=split(get(n.style,"background-position","0% 0%"));float ox=0,oy=0;if(!position.empty())ox=position[0].ends_with('%')?len(position[0],w-bw):len(position[0]);if(position.size()>1)oy=position[1].ends_with('%')?len(position[1],h-bh):len(position[1]);
        // The ordinary linear gradient fits inside one background tile.
        // Native brushes avoid regenerating a per-pixel CPU bitmap as its
        // position/colors animate. Tiled/conic/radial cases keep the reference.
        if(linear && bw>0 && bh>0 && ox<=0 && oy<=0 && w-ox<=bw+.001f && h-oy<=bh+.001f){
            std::vector<D2D1_GRADIENT_STOP> stops;bool supported=true;
            for(const auto& stop:s){supported=supported && stop.offset>=0 && stop.offset<=1 && stop.color.a==1;stops.push_back({stop.offset,stop.color});}
            if(supported){float dx=std::sin(angle*pi/180),dy=-std::cos(angle*pi/180),extent=std::abs(bw*dx)+std::abs(bh*dy);
                ComPtr<ID2D1GradientStopCollection> collection;check(target->CreateGradientStopCollection(stops.data(),UINT(stops.size()),D2D1_GAMMA_2_2,D2D1_EXTEND_MODE_CLAMP,&collection));
                auto center=D2D1::Point2F(ox+bw/2,oy+bh/2);ComPtr<ID2D1LinearGradientBrush> gradient_brush;
                check(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({center.x-dx*extent/2,center.y-dy*extent/2},{center.x+dx*extent/2,center.y+dy*extent/2}),collection.Get(),&gradient_brush));target->FillGeometry(shape,gradient_brush.Get());return;
            }
        }
        auto cache_key=std::tuple{image,get(n.style,"background-size","100% 100%"),get(n.style,"background-position","0% 0%"),w,h,scale};
        auto fill=[&](ID2D1Bitmap* bitmap){ComPtr<ID2D1BitmapBrush> bg;check(target->CreateBitmapBrush(bitmap,&bg));bg->SetTransform(D2D1::Matrix3x2F::Scale(1/scale,1/scale));target->FillGeometry(shape,bg.Get());};
        if(auto i=gradient_cache.find(cache_key);i!=gradient_cache.end()){fill(i->second.Get());return;}
        unsigned iw=unsigned(std::ceil(w*scale)),ih=unsigned(std::ceil(h*scale));if(!iw || !ih)return;if(std::uint64_t(iw)*ih>16777216)fail("gradient bitmap bound");std::vector<std::uint8_t> pixels(std::size_t(iw)*ih*4);float dx=std::sin(angle*pi/180),dy=-std::cos(angle*pi/180),extent=std::abs(bw*dx)+std::abs(bh*dy);
        for(unsigned y=0;y<ih;++y)for(unsigned x=0;x<iw;++x){float px=(x+.5f)/scale-ox,py=(y+.5f)/scale-oy;px=std::fmod(std::fmod(px,bw)+bw,bw);py=std::fmod(std::fmod(py,bh)+bh,bh);float t=linear?((px-bw/2)*dx+(py-bh/2)*dy)/extent+.5f:radial?std::hypot(px-bw/2,py-bh/2)/std::hypot(bw/2,bh/2):(std::atan2(px-bw/2,-(py-bh/2))*180/pi-angle)/360;
            if(conic)t=std::fmod(t+2,1.f);auto c=sampled(s,t);auto p=pixels.data()+(std::size_t(y)*iw+x)*4;p[0]=std::uint8_t(std::clamp(c.b*c.a*255+.5f,0.f,255.f));p[1]=std::uint8_t(std::clamp(c.g*c.a*255+.5f,0.f,255.f));p[2]=std::uint8_t(std::clamp(c.r*c.a*255+.5f,0.f,255.f));p[3]=std::uint8_t(std::clamp(c.a*255+.5f,0.f,255.f));}
        ComPtr<ID2D1Bitmap> bitmap;check(target->CreateBitmap({iw,ih},pixels.data(),iw*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&bitmap));fill(bitmap.Get());if(gradient_cache.size()>=32 || gradient_cache_bytes+pixels.size()>32*1024*1024){gradient_cache.clear();gradient_cache_bytes=0;}if(pixels.size()<=32*1024*1024){gradient_cache_bytes+=pixels.size();gradient_cache.emplace(std::move(cache_key),bitmap);}
    }
    void projective(const ProbeSceneNode& node,const D2D1::Matrix3x2F& parent){
        // Plane projection for preserve-3d descendants of perspective/rotateY.
        // Rasterize the whole subtree first, then inverse-map with bilinear
        // premultiplied samples. This also preserves group opacity and order.
        auto t=get(node.style,"transform");static const std::regex grammar(R"(perspective\(\s*([\d.]+)px\s*\)\s+rotateY\(\s*([-\d.]+)(deg)?\s*\))");std::smatch m;if(!std::regex_search(t,m,grammar))fail("3D transform outside perspective rotateY plane subset");float distance=num(m[1]),angle=num(m[2])*(m[3].matched?pi/180:1);auto f=ProbeSceneFrame{};f.root=node;f.root.box[0]=f.root.box[1]=0;f.root.style.erase("transform");const std::array<std::uint64_t,3> key{node.identity.view_epoch,node.identity.slot,node.identity.generation};
        std::optional<ProbeSceneBitmap> uncached;auto it=projected.find(key);if(it==projected.end() || it->second.node!=f.root || it->second.scale!=scale){SceneRasterProbe nested;auto pixels=nested.raster(f,scale);if(pixels.rgba.size()>16*1024*1024)uncached=std::move(pixels);else {if(projected.size()>=16 || projected_bytes+pixels.rgba.size()>16*1024*1024){projected.clear();projected_bytes=0;}it=projected.find(key);if(it!=projected.end()){projected_bytes-=it->second.image.rgba.size();projected.erase(it);}projected_bytes+=pixels.rgba.size();it=projected.emplace(key,ProjectiveContent{f.root,scale,std::move(pixels)}).first;}}const auto& src=uncached?*uncached:it->second.image;const float w=node.box[2],h=node.box[3];auto o=origin(node.style,w,h);float co=std::cos(angle),si=std::sin(angle);if(std::abs(co)<1e-5)return;
        // Explicit projective subtree already needs flattening in the reference.
        // Reuse that bounded source, but let the GPU perform the inverse sampling.
        if(gpu_target && device_context && 1-si*o[0]/distance>0 && 1+si*(w-o[0])/distance>0){
            ComPtr<ID2D1Bitmap> temporary;auto& bitmap=uncached?temporary:it->second.bitmap;
            if(!bitmap){++work.gpu_plane_uploads;std::vector<std::uint8_t> bytes(src.rgba.size());for(std::size_t i=0;i<bytes.size();i+=4){const unsigned a=src.rgba[i+3];bytes[i]=std::uint8_t((unsigned(src.rgba[i+2])*a+127)/255);bytes[i+1]=std::uint8_t((unsigned(src.rgba[i+1])*a+127)/255);bytes[i+2]=std::uint8_t((unsigned(src.rgba[i])*a+127)/255);bytes[i+3]=std::uint8_t(a);}check(target->CreateBitmap({src.width,src.height},bytes.data(),src.width*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&bitmap));}
            const float k=si/distance;const D2D1_MATRIX_4X4_F projection{co+o[0]*k,o[1]*k,0,k, 0,1,0,0, 0,0,1,0, o[0]*(1-co-o[0]*k),-o[1]*o[0]*k,0,1-o[0]*k};
            const D2D1_RECT_F destination{0,0,w,h},source{0,0,w*scale,h*scale};target->SetTransform(D2D1::Matrix3x2F::Translation(node.box[0],node.box[1])*parent);
            ++work.gpu_projective_draws;device_context->DrawBitmap(bitmap.Get(),&destination,1,D2D1_INTERPOLATION_MODE_LINEAR,&source,&projection);target->SetTransform(parent);return;
        }
        float left=1e7,top=1e7,right=-1e7,bottom=-1e7;for(auto c:std::array<std::array<float,2>,4>{{{0,0},{w,0},{w,h},{0,h}}}){float z=1+si*(c[0]-o[0])/distance,px=o[0]+co*(c[0]-o[0])/z,py=o[1]+(c[1]-o[1])/z;left=std::min(left,px);top=std::min(top,py);right=std::max(right,px);bottom=std::max(bottom,py);}unsigned dw=unsigned(std::ceil((right-left)*scale)),dh=unsigned(std::ceil((bottom-top)*scale));if(!dw || !dh || std::uint64_t(dw)*dh>16777216)fail("projective bitmap bound");
        std::vector<std::uint8_t> pixels(std::size_t(dw)*dh*4);for(unsigned y=0;y<dh;++y)for(unsigned x=0;x<dw;++x){float px=left+(x+.5f)/scale-o[0],py=top+(y+.5f)/scale-o[1],den=co-px*si/distance;if(std::abs(den)<1e-6)continue;float u=px/den,v=py*(1+si*u/distance);float sx=(u+o[0])*scale-.5f,sy=(v+o[1])*scale-.5f;if(sx<-.5f || sy<-.5f || sx>=src.width-.5f || sy>=src.height-.5f)continue;int ix=int(std::floor(sx)),iy=int(std::floor(sy));float fx=sx-ix,fy=sy-iy;std::array<float,4> sum{};for(int j=0;j<2;++j)for(int i=0;i<2;++i){auto xx=std::clamp(ix+i,0,int(src.width)-1),yy=std::clamp(iy+j,0,int(src.height)-1);auto p=src.rgba.data()+(std::size_t(yy)*src.width+xx)*4;float weight=(i?fx:1-fx)*(j?fy:1-fy),a=p[3]/255.f;sum[0]+=p[2]*a*weight;sum[1]+=p[1]*a*weight;sum[2]+=p[0]*a*weight;sum[3]+=p[3]*weight;}auto p=pixels.data()+(std::size_t(y)*dw+x)*4;for(unsigned c=0;c<4;++c)p[c]=std::uint8_t(std::clamp(sum[c]+.5f,0.f,255.f));}
        ComPtr<ID2D1Bitmap> bitmap;check(target->CreateBitmap({dw,dh},pixels.data(),dw*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&bitmap));target->SetTransform(D2D1::Matrix3x2F::Translation(node.box[0],node.box[1])*parent);target->DrawBitmap(bitmap.Get(),{left,top,left+dw/scale,top+dh/scale});target->SetTransform(parent);
    }
    struct PaintProgram {
        Style style;std::string tag,space,text;std::array<float,2> size{},pivot{};
        std::vector<std::pair<ProbeNodeIdentity,std::string>> children;
        std::vector<std::size_t> order;
        std::array<float,4> radii{},widths{};std::array<bool,4> solid_edges{};
        ComPtr<ID2D1PathGeometry> shape;bool clipped{},has_border{};std::size_t bytes{};
    };
    std::map<std::array<std::uint64_t,3>,std::shared_ptr<const PaintProgram>> paint_programs;std::size_t paint_program_bytes{};
    static bool parameter(const std::string& k){const auto& keys=scene_paint_properties();return k=="border-color" || std::find(keys.begin(),keys.end(),k)!=keys.end();}
    static bool matches(const PaintProgram& p,const ProbeSceneNode& n){
        if(p.size!=std::array{n.box[2],n.box[3]} || p.tag!=n.tag || p.space!=n.space || p.text!=n.text || p.children.size()!=n.children.size())return false;
        auto old=p.style.begin();for(const auto& [k,v]:n.style)if(!parameter(k)){if(old==p.style.end() || old->first!=k || old->second!=v)return false;++old;}if(old!=p.style.end())return false;
        for(std::size_t i=0;i<n.children.size();++i)if(p.children[i].first!=n.children[i].identity || p.children[i].second!=get(n.children[i].style,"z-index","0"))return false;return true;
    }
    std::shared_ptr<const PaintProgram> program(const ProbeSceneNode& n){
        const std::array<std::uint64_t,3> id{n.identity.view_epoch,n.identity.slot,n.identity.generation};auto it=paint_programs.find(id);
        if(it!=paint_programs.end() && matches(*it->second,n)){++work.paint_program_reuses;return it->second;}
        auto p=std::make_shared<PaintProgram>();p->tag=n.tag;p->space=n.space;p->text=n.text;p->size={n.box[2],n.box[3]};p->pivot=origin(n.style,n.box[2],n.box[3]);p->clipped=(get(n.style,"overflow")=="hidden" || get(n.style,"overflow")=="auto") || n.tag=="svg";
        for(const auto& [k,v]:n.style)if(!parameter(k)){p->style.emplace(k,v);p->bytes+=k.size()+v.size()+sizeof(Style::value_type);}
        unsigned i{};for(const auto* corner:{"top-left","top-right","bottom-right","bottom-left"})p->radii[i++]=len(get(n.style,std::string("border-")+corner+"-radius","0"),std::min(n.box[2],n.box[3]));
        i=0;for(const auto* edge:{"top","right","bottom","left"}){const auto k=std::string("border-")+edge;p->widths[i]=len(get(n.style,k+"-width","0"));p->solid_edges[i]=get(n.style,k+"-style","solid")=="solid";p->has_border=p->has_border || p->widths[i]>0;++i;}
        p->shape=round_box(n.box[2],n.box[3],p->radii);
        std::vector<float> z;for(const auto& c:n.children){auto v=get(c.style,"z-index","0");p->children.emplace_back(c.identity,v);z.push_back(v=="auto"?0:num(v));p->order.push_back(p->order.size());p->bytes+=sizeof(ProbeNodeIdentity)+v.size()+sizeof(std::size_t);}
        std::stable_sort(p->order.begin(),p->order.end(),[&](auto a,auto b){return z[a]<z[b];});p->bytes+=sizeof(PaintProgram)+p->tag.size()+p->space.size()+p->text.size();++work.paint_program_builds;
        if(p->bytes<=2*1024*1024){if(paint_programs.size()>=256 || paint_program_bytes+p->bytes>2*1024*1024){paint_programs.clear();paint_program_bytes=0;++work.paint_program_evictions;}it=paint_programs.find(id);if(it!=paint_programs.end()){paint_program_bytes-=it->second->bytes;paint_programs.erase(it);}paint_program_bytes+=p->bytes;paint_programs.emplace(id,p);}return p;
    }
    const ProbePaintValue* numeric(const ProbeSceneNode& n,unsigned i,ProbePaintValue::Kind kind)const{
        if(!gpu_target || !n.paint || !n.paint->values[i] || n.paint->values[i]->kind!=kind)return nullptr;return &*n.paint->values[i];
    }
    D2D1_COLOR_F paint_color(const ProbeSceneNode& n,unsigned i,const std::string& fallback){
        if(auto c=numeric(n,i,ProbePaintValue::Color)){++work.typed_draws;return {float(c->color[0]),float(c->color[1]),float(c->color[2]),float(c->color[3])};}return color(fallback,n.style);
    }
    std::vector<ProbeNodeIdentity> explicit_layers;
    struct CachedLayer {ProbeSceneNode key;float scale{};ComPtr<ID2D1Bitmap> bitmap;std::size_t bytes{};};
    std::map<std::array<std::uint64_t,3>,std::shared_ptr<const CachedLayer>> retained_layers;std::size_t retained_layer_bytes{};
    std::vector<std::shared_ptr<const CachedLayer>> frame_layer_leases;
    bool retained(const ProbeSceneNode& n,float opacity){
        if(!gpu_target || std::find(explicit_layers.begin(),explicit_layers.end(),n.identity)==explicit_layers.end())return false;
        auto source=n;source.box[0]=source.box[1]=0;source.style.erase("transform");source.style.erase("opacity");source.attributes.erase("style");
        if(source.paint){source.paint->values[0].reset();source.paint->values[1].reset();}
        const std::array<std::uint64_t,3> id{n.identity.view_epoch,n.identity.slot,n.identity.generation};
        auto it=retained_layers.find(id);std::shared_ptr<const CachedLayer> value;
        if(it!=retained_layers.end() && scene_paint_source_equal_probe(it->second->key,source) && it->second->scale==scale){value=it->second;++work.layer_reuses;}
        else {
            // This bounded source must contain all paint. Clipped roots do;
            // unclipped roots additionally validate descendant geometry and
            // text metrics. Unsupported projective/SVG content stays full.
            bool supported=true;ProbeSceneFrame frame;frame.root=source;
            auto inspect=[&](auto&& self,const ProbeSceneNode& c)->void{
                if((!c.space.empty() && c.space!="http://www.w3.org/1999/xhtml") || get(c.style,"transform").find("perspective(")!=std::string::npos)supported=false;
                if(!c.text.empty()){auto m=measure(c.text,c.style);float px=len(get(c.style,"padding-left","0"))+len(get(c.style,"padding-right","0"))+len(get(c.style,"border-left-width","0"))+len(get(c.style,"border-right-width","0"));
                    if(m.width+px>c.box[2]+.01f || m.height>c.box[3]+.01f)supported=false;}
                for(const auto& child:c.children)self(self,child);
            };inspect(inspect,source);
            if(get(source.style,"overflow")!="hidden"){
                SceneRasterProbe bounds;for(const auto& e:bounds.geometry(frame))if(e.rect[0]<-.01 || e.rect[1]<-.01 || e.rect[0]+e.rect[2]>n.box[2]+.01 || e.rect[1]+e.rect[3]>n.box[3]+.01)supported=false;
            }
            const unsigned w=unsigned(std::ceil((n.box[2]+4)*scale)),h=unsigned(std::ceil((n.box[3]+4)*scale));const auto bytes=std::size_t(w)*h*4;
            if(!supported || !w || !h || bytes>32*1024*1024){++work.layer_fallbacks;return false;}
            ComPtr<ID2D1BitmapRenderTarget> local;const D2D1_SIZE_U pixels{w,h};const D2D1_SIZE_F size{float(w),float(h)};
            check(target->CreateCompatibleRenderTarget(&size,&pixels,nullptr,D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE,&local));local->SetDpi(96,96);
            frame.root.box[0]=frame.root.box[1]=2;SceneRasterProbe painter;painter.paint_d2d(frame,local.Get(),scale);
            auto created=std::make_shared<CachedLayer>();created->key=std::move(source);created->scale=scale;created->bytes=bytes;check(local->GetBitmap(&created->bitmap));++work.layer_builds;
            if(retained_layers.size()>=32 || retained_layer_bytes+bytes>32*1024*1024){retained_layers.clear();retained_layer_bytes=0;++work.layer_evictions;}
            it=retained_layers.find(id);if(it!=retained_layers.end()){retained_layer_bytes-=it->second->bytes;retained_layers.erase(it);}retained_layer_bytes+=bytes;retained_layers.emplace(id,created);value=std::move(created);
        }
        frame_layer_leases.push_back(value);auto size=value->bitmap->GetSize();target->DrawBitmap(value->bitmap.Get(),{-2,-2,-2+size.width/scale,-2+size.height/scale},std::clamp(opacity,0.f,1.f),D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);return true;
    }
    void paint(const ProbeSceneNode& n){if(get(n.style,"display")=="none" || n.box[2]<=0 || n.box[3]<=0)return;const auto old=matrix();auto t=get(n.style,"transform","none");if(t.find("perspective(")!=t.npos){projective(n,old);return;}auto commands=gpu_target?program(n):nullptr;auto o=commands?commands->pivot:origin(n.style,n.box[2],n.box[3]);auto decoded=numeric(n,1,ProbePaintValue::Transform);if(decoded)++work.typed_draws;auto local=D2D1::Matrix3x2F::Translation(-o[0],-o[1])*(decoded?transform(*decoded,n.box[2],n.box[3]):transform(t,n.box[2],n.box[3],false))*D2D1::Matrix3x2F::Translation(o[0]+n.box[0],o[1]+n.box[1]);target->SetTransform(local*old);
        auto alpha=numeric(n,0,ProbePaintValue::Scalar);if(alpha)++work.typed_draws;float opacity=alpha?float(alpha->scalar.absolute+alpha->scalar.relative):len(get(n.style,"opacity","1"));if(retained(n,opacity)){target->SetTransform(old);return;}ComPtr<ID2D1Layer> group;if(opacity<1){group=layer();target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),std::clamp(opacity,0.f,1.f)),group.Get());}
        std::array<float,4> radii{};if(commands)radii=commands->radii;else {unsigned ri{};for(const auto* corner:{"top-left","top-right","bottom-right","bottom-left"})radii[ri++]=len(get(n.style,std::string("border-")+corner+"-radius","0"),std::min(n.box[2],n.box[3]));}auto shape=commands?commands->shape:round_box(n.box[2],n.box[3],radii);shadow(n,radii);auto background=paint_color(n,2,get(n.style,"background-color","transparent"));if(!gpu_target || background.a>0){solid(background);target->FillGeometry(shape.Get(),brush.Get());}gradient(n,shape.Get());
        bool has_border=commands?commands->has_border:false;if(!commands)for(const auto* edge:{"top","right","bottom","left"})has_border=has_border || len(get(n.style,std::string("border-")+edge+"-width","0"))>0;
        const auto current=matrix();const bool geometry_border=gpu_target && std::hypot(current._11,current._12)+std::hypot(current._21,current._22)<=16;
        if(has_border){ComPtr<ID2D1Layer> border_clip;if(!geometry_border){border_clip=layer();target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),shape.Get()),border_clip.Get());}
        bool uniform=gpu_target;const auto border_width=commands?commands->widths[0]:len(get(n.style,"border-top-width","0"));const auto border_color=get(n.style,"border-top-color","currentcolor");
        for(const auto* edge:{"top","right","bottom","left"})uniform=uniform && len(get(n.style,std::string("border-")+edge+"-width","0"))==border_width && get(n.style,std::string("border-")+edge+"-color","currentcolor")==border_color && get(n.style,std::string("border-")+edge+"-style","solid")=="solid";
        if(uniform){++work.uniform_border_draws;solid(paint_color(n,4,border_color));if(geometry_border){auto fill=border_fill(n,radii,shape.Get(),border_width,4);target->FillGeometry(fill.Get(),brush.Get());}else target->DrawGeometry(shape.Get(),brush.Get(),border_width*2);}
        else {++work.fallback_border_draws;unsigned index{};for(const auto* side:{"top","right","bottom","left"}){auto key=std::string("border-")+side;float width=commands?commands->widths[index]:len(get(n.style,key+"-width","0"));if(width<=0 || (commands?!commands->solid_edges[index]:get(n.style,key+"-style","solid")!="solid")){++index;continue;}solid(paint_color(n,4+index,get(n.style,key+"-color","currentcolor")));
            // Side wedges clip a ring so joins and circular borders retain
            // corner coverage without extending into the content.
            if(geometry_border){auto fill=border_fill(n,radii,shape.Get(),width,index);target->FillGeometry(fill.Get(),brush.Get());}
            else {auto wedge=border_wedge(n,index);ComPtr<ID2D1Layer> side_clip;side_clip=layer();target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),wedge.Get()),side_clip.Get());target->DrawGeometry(shape.Get(),brush.Get(),width*2);target->PopLayer();}++index;}
        }if(border_clip)target->PopLayer();}bool clipped=commands?commands->clipped:(get(n.style,"overflow")=="hidden" || get(n.style,"overflow")=="auto") || n.tag=="svg";ComPtr<ID2D1Layer> clip;if(clipped){clip=layer();target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),shape.Get()),clip.Get());}
        if(n.space=="http://www.w3.org/2000/svg"){target->SetTransform(svg_viewport(n)*matrix());for(const auto& c:n.children)svg(c,n);}else {draw_text(n);if(commands){for(auto index:commands->order)paint(n.children[index]);}else {std::vector<const ProbeSceneNode*> ordered;for(const auto& c:n.children)ordered.push_back(&c);std::stable_sort(ordered.begin(),ordered.end(),[](auto a,auto b){auto z=[](auto n){auto v=get(n->style,"z-index","0");return v=="auto"?0:num(v);};return z(a)<z(b);});for(auto c:ordered)paint(*c);}}
        if(clipped)target->PopLayer();if(group)target->PopLayer();target->SetTransform(old);
    }
    ProbeSceneBitmap raster(const ProbeSceneFrame& f,float dpi){if(dpi<=0 || dpi>4)fail("raster scale");gpu_target=false;device_context.Reset();scale=dpi;unsigned w=unsigned(std::ceil(f.root.box[2]*scale)),h=unsigned(std::ceil(f.root.box[3]*scale));if(!w || !h || std::uint64_t(w)*h>16777216)fail("scene backing capacity");if(!backing || backing_width!=w || backing_height!=h){brush.Reset();target.Reset();backing.Reset();layers.clear();color_text.clear();color_text_bytes=0;gradient_cache.clear();gradient_cache_bytes=0;shadows.clear();shadow_bytes=0;backing_width=w;backing_height=h;check(wic->CreateBitmap(w,h,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,&backing));check(factory->CreateWicBitmapRenderTarget(backing.Get(),D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&target));check(target->CreateSolidColorBrush(D2D1::ColorF(0,0,0,1),&brush));target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);}layer_cursor=0;target->BeginDraw();target->Clear(D2D1::ColorF(0,0,0,0));target->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale));try{paint(f.root);}catch(...){target->EndDraw();throw;}check(target->EndDraw());ProbeSceneBitmap out{w,h,std::vector<std::uint8_t>(std::size_t(w)*h*4)};WICRect rect{0,0,int(w),int(h)};ComPtr<IWICBitmapLock> lock;check(backing->Lock(&rect,WICBitmapLockRead,&lock));UINT stride{},count{};BYTE* data{};check(lock->GetStride(&stride));check(lock->GetDataPointer(&count,&data));if(count<std::size_t(h-1)*stride+w*4)fail("WIC lock size");for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){auto p=data+std::size_t(y)*stride+x*4,q=out.rgba.data()+(std::size_t(y)*w+x)*4;unsigned a=p[3];q[3]=std::uint8_t(a);if(a==255){q[0]=p[2];q[1]=p[1];q[2]=p[0];continue;}for(unsigned c=0;c<3;++c)q[c]=a?std::uint8_t(std::min(255u,(unsigned(p[2-c])*255+a/2)/a)):0;}return out;}
};
void SceneRasterProbe::paint_d2d_damage(const ProbeSceneFrame& f,ID2D1RenderTarget* target,float scale,std::array<float,4> damage,std::optional<std::array<float,4>> clip){auto& i=*impl_;i.require_owner();if(!target || !std::isfinite(scale) || scale<=0 || scale>4)fail("GPU target/scale");if(i.target.Get()!=target){ComPtr<ID2D1Factory> factory;target->GetFactory(&factory);if(factory.Get()!=i.factory.Get()){i.paint_programs.clear();i.paint_program_bytes=0;i.round_cache.clear();i.border_wedges.clear();i.border_fills.clear();i.path_cache.clear();i.path_cache_bytes=0;i.shapes.clear();i.shape_bytes=0;i.strokes.clear();i.stroke_bytes=0;i.factory=std::move(factory);}for(auto& [key,content]:i.projected)content.bitmap.Reset();i.retained_layers.clear();i.retained_layer_bytes=0;i.device_context.Reset();target->QueryInterface(IID_PPV_ARGS(&i.device_context));i.backing.Reset();i.brush.Reset();i.layers.clear();i.color_text.clear();i.color_text_bytes=0;i.gradient_cache.clear();i.gradient_cache_bytes=0;i.shadows.clear();i.shadow_bytes=0;i.target=target;check(target->CreateSolidColorBrush(D2D1::ColorF(0,0,0,1),&i.brush));}i.gpu_target=true;i.frame_layer_leases.clear();i.layer_cursor=0;i.scale=scale;target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);target->BeginDraw();target->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale));target->PushAxisAlignedClip({damage[0],damage[1],damage[2],damage[3]},D2D1_ANTIALIAS_MODE_ALIASED);target->Clear(D2D1::ColorF(0,0,0,0));if(clip)target->PushAxisAlignedClip({(*clip)[0],(*clip)[1],(*clip)[2],(*clip)[3]},D2D1_ANTIALIAS_MODE_ALIASED);try{i.paint(f.root);}catch(...){if(clip)target->PopAxisAlignedClip();target->PopAxisAlignedClip();target->EndDraw();throw;}if(clip)target->PopAxisAlignedClip();target->PopAxisAlignedClip();check(target->EndDraw());++i.work.partial_paints;}
void SceneRasterProbe::paint_d2d(const ProbeSceneFrame& f,ID2D1RenderTarget* target,float scale,std::optional<std::array<float,4>> clip){if(!target)fail("GPU target");auto size=target->GetSize();paint_d2d_damage(f,target,scale,{0,0,size.width/scale,size.height/scale},clip);--impl_->work.partial_paints;}
void SceneRasterProbe::set_layers(std::vector<ProbeNodeIdentity> values){impl_->require_owner();if(values.size()>32)fail("explicit layer count");for(auto id:values)if(!id.generation || !id.view_epoch)fail("explicit layer identity");impl_->explicit_layers=std::move(values);}
SceneRasterProbe::Work SceneRasterProbe::work()const{impl_->require_owner();return impl_->work;}
SceneRasterProbe::SceneRasterProbe():impl_(std::make_unique<Impl>()){}SceneRasterProbe::~SceneRasterProbe()=default;
std::array<double,6> SceneRasterProbe::transform_matrix(const ProbeSceneNode& n)const{impl_->require_owner();auto m=transform(get(n.style,"transform","none"),n.box[2],n.box[3],false);return {m._11,m._12,m._21,m._22,m._31,m._32};}
ProbeSceneTextMetrics SceneRasterProbe::measure(std::string_view text,const Style& s){impl_->require_owner();return impl_->measure(text,s);}ProbeSceneBitmap SceneRasterProbe::raster(const ProbeSceneFrame& f,float scale){impl_->require_owner();return impl_->raster(f,scale);}
ProbeNodeIdentity SceneRasterProbe::hit_test(const ProbeSceneFrame& f,float x,float y)const{
    impl_->require_owner();ProbeNodeIdentity result{};
    auto svg_visit=[&](auto&& self,const ProbeSceneNode& n,Plane parent,const ProbeSceneNode& root)->void{
        if(get(n.style,"display")=="none" || n.tag=="defs" || n.tag=="clipPath")return;
        const auto t=n.attributes.contains("transform")?n.attributes.at("transform"):get(n.style,"transform");const auto m=parent*affine_plane(transform(t,0,0,true));Plane inverse;if(!m.inverse(inverse))return;auto p=inverse.point(x,y);
        auto clip=impl_->svg_clip(n,root);BOOL inside{};if(clip){check(clip->FillContainsPoint({float(p[0]),float(p[1])},nullptr,&inside));if(!inside)return;}
        auto shape=impl_->svg_shape(n);
        if(shape && get(n.style,"pointer-events")!="none"){const auto pointer=get(n.style,"pointer-events","visiblePainted");const bool painted=pointer=="auto"||pointer=="painted"||pointer=="visiblePainted";const bool fill=pointer=="all"||pointer=="fill"||pointer=="visibleFill"||pointer=="visible"||(painted&&get(n.style,"fill","#000000")!="none");const bool stroke=pointer=="all"||pointer=="stroke"||pointer=="visibleStroke"||pointer=="visible"||(painted&&get(n.style,"stroke","none")!="none");if(fill && n.tag!="line")check(shape->FillContainsPoint({float(p[0]),float(p[1])},nullptr,&inside));if(!inside && stroke){auto style=impl_->svg_stroke(n);check(shape->StrokeContainsPoint({float(p[0]),float(p[1])},len(get(n.style,"stroke-width","1")),style.Get(),nullptr,&inside));}if(inside)result=n.identity;}
        if(n.tag=="text" && get(n.style,"pointer-events")!="none"){auto metrics=impl_->measure(n.text,n.style);float tx=n.attributes.contains("x")?len(n.attributes.at("x")):0,ty=n.attributes.contains("y")?len(n.attributes.at("y")):0;auto anchor=get(n.style,"text-anchor","start");if(anchor=="middle")tx-=metrics.width/2;if(anchor=="end")tx-=metrics.width;ty-=metrics.baseline;if(p[0]>=tx && p[0]<=tx+metrics.width && p[1]>=ty && p[1]<=ty+metrics.height)result=n.identity;}
        for(const auto& c:n.children)self(self,c,m,root);
    };
    auto visit=[&](auto&& self,const ProbeSceneNode& n,Plane parent)->void{if(get(n.style,"display")=="none")return;auto m=parent*plane(n);Plane inverse;if(!m.inverse(inverse))return;auto p=inverse.point(x,y);const bool inside=p[0]>=0 && p[0]<n.box[2] && p[1]>=0 && p[1]<n.box[3];if(!inside && ((get(n.style,"overflow")=="hidden" || get(n.style,"overflow")=="auto") || n.tag=="svg"))return;if(inside && n.tag!="#text" && get(n.style,"pointer-events")!="none")result=n.identity;
        if(n.space=="http://www.w3.org/2000/svg"){auto viewport=m*affine_plane(svg_viewport(n));for(const auto& c:n.children)svg_visit(svg_visit,c,viewport,n);return;}
        std::vector<const ProbeSceneNode*> order;for(const auto& c:n.children)order.push_back(&c);std::stable_sort(order.begin(),order.end(),[](auto a,auto b){return len(get(a->style,"z-index","0"))<len(get(b->style,"z-index","0"));});for(auto c:order)self(self,*c,m);
    };visit(visit,f.root,Plane{});return result;
}
std::vector<ProbeElementGeometry> SceneRasterProbe::geometry(const ProbeSceneFrame& f,bool svg_descendants)const{
    impl_->require_owner();std::vector<ProbeElementGeometry> out;
    // A group may be queried by several ancestors and then by the flat walk.
    // Memoize local bounds only inside this solve; no scene pointer survives.
    std::unordered_map<const ProbeSceneNode*,D2D1_RECT_F> local_bounds;
    auto bounds=[&](auto&& self,const ProbeSceneNode& n)->D2D1_RECT_F{if(auto it=local_bounds.find(&n);it!=local_bounds.end())return it->second;D2D1_RECT_F r{1e10f,1e10f,-1e10f,-1e10f};auto shape=impl_->svg_shape(n);if(shape)check(shape->GetBounds(nullptr,&r));
        if(n.tag=="text"){auto layout=impl_->text_layout(n.text,n.style);DWRITE_TEXT_METRICS metrics{};check(layout->GetMetrics(&metrics));DWRITE_LINE_METRICS line{};UINT count{};check(layout->GetLineMetrics(&line,1,&count));float tx=n.attributes.contains("x")?len(n.attributes.at("x")):0,ty=n.attributes.contains("y")?len(n.attributes.at("y")):0;if(n.attributes.contains("dy"))ty+=len(n.attributes.at("dy"),len(get(n.style,"font-size","16px")));auto anchor=get(n.style,"text-anchor","start");if(anchor=="middle")tx-=metrics.width/2;if(anchor=="end")tx-=metrics.width;ty-=get(n.style,"dominant-baseline")=="central"?metrics.height/2:line.baseline;r={tx,ty,tx+metrics.width,ty+metrics.height};}
        for(const auto& c:n.children)if(c.tag!="defs"&&c.tag!="clipPath"){auto q=self(self,c);auto t=transform(c.attributes.contains("transform")?c.attributes.at("transform"):get(c.style,"transform"),0,0,true);if(q.right>=q.left)for(auto p:std::array<D2D1_POINT_2F,4>{{{q.left,q.top},{q.right,q.top},{q.right,q.bottom},{q.left,q.bottom}}}){auto v=t.TransformPoint(p);r.left=std::min(r.left,v.x);r.top=std::min(r.top,v.y);r.right=std::max(r.right,v.x);r.bottom=std::max(r.bottom,v.y);}}local_bounds.emplace(&n,r);return r;};
    auto append=[&](const ProbeSceneNode& n,Plane m,std::array<double,4> local,ProbeNodeIdentity offset_parent){double left=1e10,top=1e10,right=-1e10,bottom=-1e10;for(auto p:std::array<std::array<double,2>,4>{{{local[0],local[1]},{local[0]+local[2],local[1]},{local[0]+local[2],local[1]+local[3]},{local[0],local[1]+local[3]}}}){auto q=m.point(p[0],p[1]);left=std::min(left,q[0]);top=std::min(top,q[1]);right=std::max(right,q[0]);bottom=std::max(bottom,q[1]);}ProbeElementGeometry e;e.identity=n.identity;e.rect={left,top,right-left,bottom-top};e.client_rects={e.rect};e.client_width=std::max(0.f,n.box[2]-len(get(n.style,"border-left-width","0"))-len(get(n.style,"border-right-width","0")));e.client_height=std::max(0.f,n.box[3]-len(get(n.style,"border-top-width","0"))-len(get(n.style,"border-bottom-width","0")));e.scroll_width=e.client_width;e.scroll_height=e.client_height;e.offset={n.box[0],n.box[1],n.box[2],n.box[3]};e.offset_parent=offset_parent;e.matrix={m.m[0],m.m[3],m.m[1],m.m[4],m.m[2],m.m[5]};out.push_back(e);};
    auto svg_visit=[&](auto&& self,const ProbeSceneNode& n,Plane parent)->void{if(n.tag=="defs"||n.tag=="clipPath")return;auto t=transform(n.attributes.contains("transform")?n.attributes.at("transform"):get(n.style,"transform"),0,0,true);auto m=parent*affine_plane(t);auto b=bounds(bounds,n);if(b.right>=b.left)append(n,m,{b.left,b.top,b.right-b.left,b.bottom-b.top},{});for(const auto& c:n.children)self(self,c,m);};
    auto visit=[&](auto&& self,const ProbeSceneNode& n,Plane parent,ProbeNodeIdentity offset_parent,double ox,double oy)->void{const auto m=parent*plane(n);if(n.tag!="#text"){append(n,m,{0,0,n.box[2],n.box[3]},offset_parent);out.back().offset[0]+=ox;out.back().offset[1]+=oy;}
        if(n.space=="http://www.w3.org/2000/svg"){auto viewport=m*affine_plane(svg_viewport(n));if(svg_descendants){out.back().matrix={viewport.m[0],viewport.m[3],viewport.m[1],viewport.m[4],viewport.m[2],viewport.m[5]};for(const auto& c:n.children)svg_visit(svg_visit,c,viewport);}return;}
        const bool positioned=get(n.style,"position","static")!="static" || get(n.style,"transform","none")!="none" || n.identity==f.root.identity;for(const auto& c:n.children)self(self,c,m,positioned?n.identity:offset_parent,positioned?0:ox+n.box[0],positioned?0:oy+n.box[1]);};visit(visit,f.root,Plane{},{},0,0);return out;
}
}
