#define NOMINMAX
#include "caseelephant/canvas_probe.hpp"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <stdexcept>
#include <algorithm>

namespace caseelephant {
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("CE_CANVAS_D2D: HRESULT="+std::to_string(static_cast<unsigned long>(hr)));}
class Raster final:public ProbeCanvasRaster {
    std::uint32_t w_,h_;bool alpha_;
    ComPtr<IWICBitmap> bitmap_;
    ComPtr<ID2D1RenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
public:
    Raster(ID2D1Factory* factory,IWICImagingFactory* wic,std::uint32_t w,std::uint32_t h,bool a):w_(w),h_(h),alpha_(a){
        if(!w || !h)return;
        check(wic->CreateBitmap(w,h,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,&bitmap_));
        const auto props=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
        check(factory->CreateWicBitmapRenderTarget(bitmap_.Get(),props,&target_));
        check(target_->CreateSolidColorBrush(D2D1::ColorF(0,0,0,1),&brush_));
        target_->BeginDraw();target_->Clear(D2D1::ColorF(0,0,0,a?0.f:1.f));check(target_->EndDraw());
    }
    void draw(std::span<const ProbeCanvasCommand> commands)override{
        if(!target_)return;
        target_->BeginDraw();
        for(const auto& c:commands){const auto& g=c.geometry;
            brush_->SetColor(D2D1::ColorF(c.srgb[0],c.srgb[1],c.srgb[2],c.srgb[3]));
            if(c.kind==ProbeCanvasCommand::Kind::FillCircle)target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(float(g[0]),float(g[1])),float(g[2]),float(g[2])),brush_.Get());
            else{const auto r=D2D1::RectF(float(g[0]),float(g[1]),float(g[0]+g[2]),float(g[1]+g[3]));
                if(c.kind==ProbeCanvasCommand::Kind::FillRect)target_->FillRectangle(r,brush_.Get());
                else{target_->PushAxisAlignedClip(r,D2D1_ANTIALIAS_MODE_ALIASED);target_->Clear(D2D1::ColorF(0,0,0,alpha_?0.f:1.f));target_->PopAxisAlignedClip();}
            }
        }
        check(target_->EndDraw());
    }
    std::vector<std::uint8_t> rgba_straight()override{
        std::vector<std::uint8_t> result(std::size_t(w_)*h_*4);
        if(!bitmap_)return result;
        WICRect rect{0,0,int(w_),int(h_)};ComPtr<IWICBitmapLock> lock;
        check(bitmap_->Lock(&rect,WICBitmapLockRead,&lock));UINT stride{},count{};BYTE* data{};
        check(lock->GetStride(&stride));check(lock->GetDataPointer(&count,&data));
        if(count<std::size_t(h_-1)*stride+w_*4)throw std::runtime_error("CE_CANVAS_WIC: short lock");
        for(std::uint32_t y=0;y<h_;++y)for(std::uint32_t x=0;x<w_;++x){
            const auto p=data+std::size_t(y)*stride+x*4;auto q=result.data()+(std::size_t(y)*w_+x)*4;
            const unsigned a=p[3];q[3]=std::uint8_t(a);
            // D2D/WIC stores encoded sRGB premultiplied BGRA. Unpremultiply
            // BEFORE the existing UI renderer's sRGB transfer decode.
            for(unsigned c=0;c<3;++c)q[c]=a?std::uint8_t(std::min(255u,(unsigned(p[2-c])*255+a/2)/a)):0;
        }
        return result;
    }
};
}
ProbeCanvasFactory make_d2d_canvas_factory_probe(){
    ComPtr<ID2D1Factory> factory;check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf()));
    ComPtr<IWICImagingFactory> wic;check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)));
    return [factory,wic](std::uint32_t w,std::uint32_t h,bool a){return std::make_unique<Raster>(factory.Get(),wic.Get(),w,h,a);};
}
}
