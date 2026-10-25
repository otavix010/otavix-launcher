#include "photo.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "profile.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#endif

namespace launcher {

namespace {

constexpr unsigned kLongest = 1920;          // px on the longest side
constexpr size_t kMaxBytes = 1'800'000;      // the server takes up to 1.9 MB a picture

#ifndef _WIN32
bool ReadAll(const fs::path& file, std::string& out) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool JpegOrPng(const std::string& b) {
    return (b.size() > 3 && uint8_t(b[0]) == 0xFF && uint8_t(b[1]) == 0xD8 && uint8_t(b[2]) == 0xFF) ||
           (b.size() > 4 && uint8_t(b[0]) == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G');
}
#endif

#ifdef _WIN32
// COM on this thread for the call (SDL's dialogs may have it on already, in either model)
struct Com {
    HRESULT hr;
    Com() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~Com() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

template <class T>
struct Ref {
    T* p = nullptr;
    ~Ref() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() { return p; }
    operator T*() { return p; }
};

std::string Hr(const char* what, HRESULT hr) {
    char b[96];
    std::snprintf(b, sizeof(b), "%s (0x%08lX)", what, static_cast<unsigned long>(hr));
    return b;
}

// a picture, scaled down when larger than kLongest, as a JPEG; the quality goes down until it fits
bool EncodeJpeg(IWICImagingFactory* wic, IWICBitmapSource* src, Photo& out, std::string& error) {
    UINT w = 0, h = 0;
    src->GetSize(&w, &h);
    if (!w || !h) {
        error = "imagem vazia";
        return false;
    }
    UINT nw = w, nh = h;
    Ref<IWICBitmapScaler> scaler;
    IWICBitmapSource* use = src;
    if (std::max(w, h) > kLongest) {
        double k = double(kLongest) / double(std::max(w, h));
        nw = std::max(1u, UINT(w * k + 0.5)), nh = std::max(1u, UINT(h * k + 0.5));
        HRESULT hr = wic->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) hr = scaler->Initialize(src, nw, nh, WICBitmapInterpolationModeHighQualityCubic);
        if (FAILED(hr)) {
            error = Hr("não deu para reduzir a imagem", hr);
            return false;
        }
        use = scaler;
    }
    Ref<IWICFormatConverter> bgr;
    HRESULT hr = wic->CreateFormatConverter(&bgr);
    if (SUCCEEDED(hr))
        hr = bgr->Initialize(use, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        error = Hr("formato de imagem não suportado", hr);
        return false;
    }
    for (float quality : {0.88f, 0.75f, 0.6f}) {
        Ref<IStream> stream;
        Ref<IWICBitmapEncoder> enc;
        Ref<IWICBitmapFrameEncode> frame;
        Ref<IPropertyBag2> bag;
        hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
        if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc);
        if (SUCCEEDED(hr)) hr = enc->Initialize(stream, WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, &bag);
        if (SUCCEEDED(hr)) {
            PROPBAG2 opt{};
            opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_R4;
            v.fltVal = quality;
            bag->Write(1, &opt, &v);
            hr = frame->Initialize(bag);
        }
        if (SUCCEEDED(hr)) hr = frame->SetSize(nw, nh);
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&pf);
        if (SUCCEEDED(hr)) hr = frame->WriteSource(bgr, nullptr);
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = enc->Commit();
        HGLOBAL mem = nullptr;
        if (SUCCEEDED(hr)) hr = GetHGlobalFromStream(stream, &mem);
        STATSTG st{};
        if (SUCCEEDED(hr)) hr = stream->Stat(&st, STATFLAG_NONAME);
        if (FAILED(hr)) {
            error = Hr("não deu para gravar o JPEG", hr);
            return false;
        }
        const void* p = GlobalLock(mem);
        out.bytes.assign(static_cast<const char*>(p), size_t(st.cbSize.QuadPart));
        GlobalUnlock(mem);
        out.width = int(nw), out.height = int(nh);
        if (out.bytes.size() <= kMaxBytes) return true;
    }
    error = "imagem grande demais";
    return false;
}
#endif

}  // namespace

bool PhotoFromFile(const fs::path& file, Photo& out, std::string& error) {
    out = {};
    out.name = (file.stem().empty() ? std::string("foto") : U8(file.stem())) + ".jpg";
#ifdef _WIN32
    Com com;
    Ref<IWICImagingFactory> wic;
    Ref<IWICBitmapDecoder> dec;
    Ref<IWICBitmapFrameDecode> frame;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic.p));
    if (SUCCEEDED(hr))
        hr = wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) return EncodeJpeg(wic, frame, out, error);
    error = Hr("não é uma imagem que o Windows abra", hr);
    return false;
#else
    std::string b;
    if (!ReadAll(file, b) || !JpegOrPng(b)) {
        error = "só JPEG ou PNG";
        return false;
    }
    if (b.size() > kMaxBytes) {
        error = "imagem grande demais";
        return false;
    }
    out.name = U8(file.filename());
    out.bytes = std::move(b);
    return true;
#endif
}

bool PhotoFromClipboard(Photo& out, std::string& error) {
    out = {};
    error.clear();
#ifdef _WIN32
    Com com;
    Ref<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic.p));
    if (FAILED(hr)) {
        error = Hr("WIC indisponível", hr);
        return false;
    }
    if (!IsClipboardFormatAvailable(CF_BITMAP) || !OpenClipboard(nullptr)) return false;
    // a copy of the pixels before the clipboard goes back to whoever owns it
    Ref<IWICBitmap> held, copy;
    HBITMAP hb = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
    hr = hb ? wic->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapIgnoreAlpha, &held) : E_FAIL;
    if (SUCCEEDED(hr)) hr = wic->CreateBitmapFromSource(held, WICBitmapCacheOnLoad, &copy);
    CloseClipboard();
    if (FAILED(hr)) {
        error = Hr("não deu para ler o print colado", hr);
        return false;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char name[48];
    std::snprintf(name, sizeof(name), "print_%02d%02d%02d.jpg", t.wHour, t.wMinute, t.wSecond);
    out.name = name;
    return EncodeJpeg(wic, copy, out, error);
#else
    return false;
#endif
}

}  // namespace launcher
