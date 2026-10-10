#define NOMINMAX
#include "../probes/gallery_scene_session.hpp"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <cstring>
using Microsoft::WRL::ComPtr;
void require(bool x,const char* s){if(!x)throw std::runtime_error(s);}
void hr(HRESULT h){require(SUCCEEDED(h),"WIC operation");}
void png(const std::filesystem::path& path,const caseelephant::ProbeSceneBitmap& image){ComPtr<IWICImagingFactory> f;hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f)));ComPtr<IWICStream> stream;hr(f->CreateStream(&stream));hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));ComPtr<IWICBitmapEncoder> encoder;hr(f->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));ComPtr<IWICBitmapFrameEncode> frame;hr(encoder->CreateNewFrame(&frame,nullptr));hr(frame->Initialize(nullptr));hr(frame->SetSize(image.width,image.height));auto format=GUID_WICPixelFormat32bppBGRA;hr(frame->SetPixelFormat(&format));require(format==GUID_WICPixelFormat32bppBGRA,"PNG format");auto bytes=image.rgba;for(std::size_t i=0;i<bytes.size();i+=4)std::swap(bytes[i],bytes[i+2]);hr(frame->WritePixels(image.height,image.width*4,UINT(image.rgba.size()),bytes.data()));hr(frame->Commit());hr(encoder->Commit());}
#include "scene_visual_compare.hpp"
int main(int argc,char** argv){try{hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED));auto out=std::filesystem::path(argc>1?argv[1]:"artifacts/scene-gallery");std::filesystem::create_directories(out);std::ostringstream report;report<<"{\"format\":\"scene-gallery-native-v1\",\"seed\":3458213392,\"dpi\":2,\"frames_per_case\":121,\"cases\":[";unsigned cases{};
    for(unsigned group=0;group<2;++group)for(unsigned index=0;index<(group?8u:26u);++index){const auto id=std::string(group?"d3-":"css-")+(index<10?"0":"")+std::to_string(index);std::cout<<id<<"..."<<std::flush;GallerySceneSession s(group!=0,index,group?384.f:387.f);const auto baseline=s.vm.stats();std::vector<std::uint8_t> previous;std::size_t changed{};unsigned captures{};double worst_mean{},worst_iou=1;
        for(unsigned frame=0;frame<=120;++frame){const double time=frame*(1000./60);s.step(time);if(!group && index>=4 && index<=7 && (frame==18 || frame==72)){s.input(index==7?caseelephant::ProbeMouseType::Click:caseelephant::ProbeMouseType::Move,frame==72 && index!=7?-10.f:s.width/2,frame==72 && index!=7?-10.f:60.f);s.step(time);}
            if(frame==24 || frame==63 || frame==120){const auto& image=s.bitmap();require(image.width==unsigned(s.width*2) && image.height==unsigned(s.height*2),"scene pixel extent");const auto ref=scene_reference(std::filesystem::path(CE_SOURCE_DIR)/"tests/fixtures/scene-gallery-reference"/(id+"-"+std::to_string(frame)+".png"));const auto diff=scene_difference(image,ref);worst_mean=std::max(worst_mean,*std::max_element(diff.mean.begin(),diff.mean.end()));worst_iou=std::min(worst_iou,diff.soft_iou);if(!diff.passes())std::cerr<<id<<" frame "<<frame<<" mean="<<worst_mean<<" IoU="<<diff.soft_iou<<'\n';require(diff.passes(),"native scene outside declared Chromium image tolerances");
                if(frame==24){auto blank=image;for(std::size_t i=0;i<blank.rgba.size();i+=4){blank.rgba[i]=5;blank.rgba[i+1]=4;blank.rgba[i+2]=3;blank.rgba[i+3]=255;}require(!scene_difference(blank,ref).passes(),"visual oracle accepted missing original content");}
                png(out/(id+"-"+std::to_string(frame)+".png"),image);std::ofstream raw(out/(id+"-"+std::to_string(frame)+".rgba"),std::ios::binary);raw.write(reinterpret_cast<const char*>(image.rgba.data()),std::streamsize(image.rgba.size()));if(!previous.empty())for(std::size_t i=0;i<image.rgba.size();i+=4)changed+=std::memcmp(image.rgba.data()+i,previous.data()+i,4)!=0;previous=image.rgba;++captures;
                std::ofstream geometry(out/(id+"-"+std::to_string(frame)+"-geometry.json"));geometry<<'[';bool comma{};for(const auto& e:s.current.elements){if(comma)geometry<<',';comma=true;geometry<<'['<<e.rect[0]<<','<<e.rect[1]<<','<<e.rect[2]<<','<<e.rect[3]<<']';}geometry<<"]\n";
            }
        }
        require(s.vm.stats().listener_exceptions==0,"React input exception");require(s.vm.eval("__ceGallery.state().errors.length")=="0","original error boundary");if((!group && index>=4 && index!=5) || (group && (index==4 || index==5)))require(changed>0,"original animated case stayed static");
        auto retained=std::move(s.cached);s.unmount();require(s.vm.stats().live_nodes==3 && s.vm.stats().canvas_contexts==0,"original unmount/reset node leak");require(!retained.rgba.empty(),"immutable bitmap lost after VM teardown");
        if(cases++)report<<',';report<<"{\"id\":\""<<id<<"\",\"execution\":\"passed\",\"captures\":"<<captures<<",\"changed_pixels\":"<<changed<<",\"live_nodes\":"<<baseline.live_nodes<<",\"peak_vm_bytes\":"<<baseline.peak_vm_allocated_bytes<<",\"worst_channel_mean\":"<<worst_mean<<",\"minimum_soft_iou\":"<<worst_iou<<'}';std::cout<<" OK (changed="<<changed<<")\n";
    }
    report<<"]}\n";std::ofstream(out/"native-report.json")<<report.str();require(cases==34,"original registry count");std::cout<<"PASS 26 original CSS + 8 D3: native owner, layout, raster, animation/input, retained values/unmount\n";CoUninitialize();return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
