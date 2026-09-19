#include "materials.hpp"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
namespace codm {
void write_png(const fs::path &path,const Image &image) {
    require(image.width>0 && image.height>0 && image.pixels.size()==size_t(image.width)*image.height*4,"Invalid image dimensions");
    require(uint64_t(image.width)*4<=UINT32_MAX,"PNG stride exceeds bound");
    struct Apartment {
        HRESULT status=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        Apartment(){require(SUCCEEDED(status) || status==RPC_E_CHANGED_MODE,"Cannot initialize Windows image encoder");}
        ~Apartment(){if(SUCCEEDED(status)) CoUninitialize();}
    } apartment;
    auto check=[&](HRESULT result,const char *operation) {require(SUCCEEDED(result),std::string(operation)+": "+pathstr(path)+" (Windows error "+hex64(uint32_t(result))+")");};
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"Cannot create PNG encoder");
    if(!path.parent_path().empty()) fs::create_directories(path.parent_path());
    ComPtr<IWICStream> stream;
    check(factory->CreateStream(&stream),"Cannot create PNG stream");
    check(stream->InitializeFromFilename(fs::absolute(path).c_str(),GENERIC_WRITE),"Cannot write PNG");
    ComPtr<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"Cannot select PNG codec");
    check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"Cannot initialize PNG stream");
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> options;
    check(encoder->CreateNewFrame(&frame,&options),"Cannot create PNG frame");
    PROPBAG2 property{};property.pstrName=const_cast<wchar_t *>(L"FilterOption");
    VARIANT value{};value.vt=VT_UI1;value.bVal=WICPngFilterSub;
    check(options->Write(1,&property,&value),"Cannot set lossless PNG filter");
    check(frame->Initialize(options.Get()),"Cannot initialize PNG frame");
    check(frame->SetSize(image.width,image.height),"Cannot set PNG size");
    auto format=GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format),"Cannot set PNG pixel format");
    require(IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA),"PNG codec cannot preserve straight 8-bit RGBA");
    // WIC's native format is straight BGRA. Swizzle bounded row batches without
    // gamma conversion or alpha premultiplication (alpha may contain packed data).
    const UINT stride=UINT(image.width)*4;
    Bytes rows(size_t(stride)*std::min(128,image.height));
    for(int y=0;y<image.height;y+=128) {
        UINT count=UINT(std::min(128,image.height-y));size_t bytes=size_t(count)*stride;
        const auto *source=image.pixels.data()+size_t(y)*stride;
        for(size_t p=0;p<bytes;p+=4) {rows[p]=source[p+2];rows[p+1]=source[p+1];rows[p+2]=source[p];rows[p+3]=source[p+3];}
        check(frame->WritePixels(count,stride,UINT(bytes),rows.data()),"Cannot encode PNG rows");
    }
    check(frame->Commit(),"Cannot finish PNG frame");
    check(encoder->Commit(),"Cannot finish PNG file");
}
}
