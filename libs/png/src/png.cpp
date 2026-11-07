#include "wrally/png/png.h"

#include <windows.h>
#include <wincodec.h>

namespace wrally::png {

namespace {

template <class T> struct Com {
    T* p = nullptr;
    ~Com()
    {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

bool Encode(const std::wstring& path, int width, int height, const unsigned char* bgr, size_t stride)
{
    Com<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;
    Com<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
        return false;
    Com<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.p, WICBitmapEncoderNoCache)))
        return false;
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> options;
    if (FAILED(encoder->CreateNewFrame(&frame, &options)) || FAILED(frame->Initialize(options.p)) ||
        FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height))))
        return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat24bppBGR) return false;
    if (FAILED(frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(stride),
                                  static_cast<UINT>(stride * static_cast<size_t>(height)), const_cast<BYTE*>(bgr))))
        return false;
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

} // namespace

bool WriteBgr(const std::string& path, int width, int height, const unsigned char* bgr, size_t stride)
{
    if (width <= 0 || height <= 0 || !bgr) return false;
    const int n = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wide.data(), n);
    wide.resize(static_cast<size_t>(n - 1));
    // COM on this thread for the call; a thread already in the other apartment model still has it.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ok = Encode(wide, width, height, bgr, stride);
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

} // namespace wrally::png
