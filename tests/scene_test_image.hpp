#pragma once
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
using Microsoft::WRL::ComPtr;
void require(bool x,const char* s){if(!x)throw std::runtime_error(s);}
void hr(HRESULT h){require(SUCCEEDED(h),"WIC operation");}
void png(const std::filesystem::path& path,const caseelephant::ProbeSceneBitmap& image){ComPtr<IWICImagingFactory> f;hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f)));ComPtr<IWICStream> stream;hr(f->CreateStream(&stream));hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));ComPtr<IWICBitmapEncoder> encoder;hr(f->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));ComPtr<IWICBitmapFrameEncode> frame;hr(encoder->CreateNewFrame(&frame,nullptr));hr(frame->Initialize(nullptr));hr(frame->SetSize(image.width,image.height));auto format=GUID_WICPixelFormat32bppBGRA;hr(frame->SetPixelFormat(&format));require(format==GUID_WICPixelFormat32bppBGRA,"PNG format");auto bytes=image.rgba;for(std::size_t i=0;i<bytes.size();i+=4)std::swap(bytes[i],bytes[i+2]);hr(frame->WritePixels(image.height,image.width*4,UINT(image.rgba.size()),bytes.data()));hr(frame->Commit());hr(encoder->Commit());}
#include "scene_visual_compare.hpp"
