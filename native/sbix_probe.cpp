#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d2d1_3.h>
#include <d3d11.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT32 kCanvasSize = 256;
constexpr DWRITE_GLYPH_IMAGE_FORMATS kPngFormat = DWRITE_GLYPH_IMAGE_FORMATS_PNG;
constexpr uint32_t kRenderSizes[] = {32, 64, 96, 128};

struct TestCase {
    const char* id;
    std::vector<uint32_t> codepoints;
    bool expectChromatic;
};

const TestCase kCases[] = {
    {"grinning-face", {0x1F600}, true},
    {"family-zwj", {0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466}, false},
    {"woman-technologist-zwj", {0x1F469, 0x200D, 0x1F4BB}, true},
};

std::string HrText(HRESULT hr) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return out.str();
}

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), length, nullptr, nullptr);
    result.resize(length - 1);
    return result;
}

void Check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(operation) + " failed with HRESULT " + HrText(hr));
    }
}

std::vector<wchar_t> ToUtf16(const std::vector<uint32_t>& codepoints) {
    std::vector<wchar_t> result;
    for (uint32_t cp : codepoints) {
        if (cp <= 0xFFFF) {
            result.push_back(static_cast<wchar_t>(cp));
        } else {
            cp -= 0x10000;
            result.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            result.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        }
    }
    return result;
}

std::string CodepointList(const std::vector<uint32_t>& codepoints) {
    std::ostringstream out;
    for (size_t i = 0; i < codepoints.size(); ++i) {
        if (i) out << ' ';
        out << "U+" << std::uppercase << std::hex << codepoints[i];
    }
    return out.str();
}

std::wstring FontFamilyName(IDWriteFontFace* face) {
    ComPtr<IDWriteFontFace3> face3;
    Check(face->QueryInterface(IID_PPV_ARGS(&face3)), "QueryInterface(IDWriteFontFace3)");
    ComPtr<IDWriteLocalizedStrings> names;
    Check(face3->GetFamilyNames(&names), "IDWriteFontFace3::GetFamilyNames");
    UINT32 length = 0;
    Check(names->GetStringLength(0, &length), "IDWriteLocalizedStrings::GetStringLength");
    std::vector<wchar_t> value(length + 1, L'\0');
    Check(names->GetString(0, value.data(), static_cast<UINT32>(value.size())),
          "IDWriteLocalizedStrings::GetString");
    return value.data();
}

struct ShapedRun {
    std::vector<UINT16> glyphIndices;
    std::vector<FLOAT> advances;
    std::vector<DWRITE_GLYPH_OFFSET> offsets;
};

ShapedRun Shape(
    IDWriteTextAnalyzer* analyzer,
    IDWriteFontFace* face,
    const std::vector<uint32_t>& codepoints,
    FLOAT emSize) {
    const std::vector<wchar_t> text = ToUtf16(codepoints);
    const UINT32 textLength = static_cast<UINT32>(text.size());
    const UINT32 maxGlyphs = textLength * 3 + 16;
    std::vector<UINT16> clusterMap(textLength);
    std::vector<DWRITE_SHAPING_TEXT_PROPERTIES> textProperties(textLength);
    std::vector<UINT16> glyphIndices(maxGlyphs);
    std::vector<DWRITE_SHAPING_GLYPH_PROPERTIES> glyphProperties(maxGlyphs);
    UINT32 actualGlyphCount = 0;
    DWRITE_SCRIPT_ANALYSIS script{};
    // Script 0 is DirectWrite's common/unspecified script bucket for these emoji sequences.
    script.script = 0;
    script.shapes = DWRITE_SCRIPT_SHAPES_DEFAULT;

    Check(analyzer->GetGlyphs(
              text.data(), textLength, face, FALSE, FALSE, &script, L"en-us", nullptr,
              nullptr, nullptr, 0, maxGlyphs, clusterMap.data(), textProperties.data(),
              glyphIndices.data(), glyphProperties.data(), &actualGlyphCount),
          "IDWriteTextAnalyzer::GetGlyphs");
    if (actualGlyphCount == 0) {
        throw std::runtime_error("DirectWrite shaped the input to zero glyphs");
    }
    glyphIndices.resize(actualGlyphCount);
    glyphProperties.resize(actualGlyphCount);

    ShapedRun shaped;
    shaped.glyphIndices = glyphIndices;
    shaped.advances.resize(actualGlyphCount);
    shaped.offsets.resize(actualGlyphCount);
    Check(analyzer->GetGlyphPlacements(
              text.data(), clusterMap.data(), textProperties.data(), textLength,
              shaped.glyphIndices.data(), glyphProperties.data(), actualGlyphCount, face,
              emSize, FALSE, FALSE, &script, L"en-us", nullptr, nullptr, 0,
              shaped.advances.data(), shaped.offsets.data()),
          "IDWriteTextAnalyzer::GetGlyphPlacements");
    return shaped;
}

struct WarpD2D {
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<ID2D1DeviceContext4> context4;
    D3D_FEATURE_LEVEL featureLevel{};
};

WarpD2D CreateWarpD2D() {
    WarpD2D result;
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    Check(D3D11CreateDevice(
              nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
              requested, ARRAYSIZE(requested), D3D11_SDK_VERSION, &result.d3dDevice,
              &result.featureLevel, &result.d3dContext),
          "D3D11CreateDevice(D3D_DRIVER_TYPE_WARP)");
    ComPtr<IDXGIDevice> dxgiDevice;
    Check(result.d3dDevice.As(&dxgiDevice), "QueryInterface(IDXGIDevice)");
    Check(D2D1CreateDevice(dxgiDevice.Get(), nullptr, &result.d2dDevice), "D2D1CreateDevice");
    Check(result.d2dDevice->CreateDeviceContext(
              D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &result.context),
          "ID2D1Device::CreateDeviceContext");
    Check(result.context.As(&result.context4), "QueryInterface(ID2D1DeviceContext4)");
    return result;
}

struct RenderTarget {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<IDXGISurface> surface;
    ComPtr<ID2D1Bitmap1> bitmap;
};

RenderTarget CreateRenderTarget(WarpD2D& warp) {
    RenderTarget result;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kCanvasSize;
    desc.Height = kCanvasSize;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Check(warp.d3dDevice->CreateTexture2D(&desc, nullptr, &result.texture),
          "ID3D11Device::CreateTexture2D(render target)");
    Check(result.texture.As(&result.surface), "QueryInterface(IDXGISurface)");

    D2D1_BITMAP_PROPERTIES1 bitmapProperties{};
    bitmapProperties.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bitmapProperties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bitmapProperties.dpiX = 96.0f;
    bitmapProperties.dpiY = 96.0f;
    bitmapProperties.bitmapOptions =
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    Check(warp.context->CreateBitmapFromDxgiSurface(
              result.surface.Get(), &bitmapProperties, &result.bitmap),
          "ID2D1DeviceContext::CreateBitmapFromDxgiSurface");

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(warp.d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &result.staging),
          "ID3D11Device::CreateTexture2D(readback)");
    return result;
}

std::vector<BYTE> ReadPixels(WarpD2D& warp, RenderTarget& target) {
    warp.context->SetTarget(nullptr);
    warp.d3dContext->CopyResource(target.staging.Get(), target.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(warp.d3dContext->Map(target.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
          "ID3D11DeviceContext::Map(readback)");
    std::vector<BYTE> pixels(kCanvasSize * kCanvasSize * 4);
    for (UINT32 y = 0; y < kCanvasSize; ++y) {
        const BYTE* row = static_cast<const BYTE*>(mapped.pData) + y * mapped.RowPitch;
        std::copy(row, row + kCanvasSize * 4, pixels.begin() + y * kCanvasSize * 4);
    }
    warp.d3dContext->Unmap(target.staging.Get(), 0);
    return pixels;
}

struct PixelStats {
    size_t nonBackground;
    size_t quantizedColors;
    size_t chromaticPixels;
    size_t chromaticColors;
};

PixelStats CheckColorPixels(const std::vector<BYTE>& pixels, bool requireChromaticPixels) {
    size_t nonBackground = 0;
    size_t chromaticPixels = 0;
    std::set<uint32_t> colors;
    std::set<uint32_t> chromaticColors;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        const BYTE b = pixels[i];
        const BYTE g = pixels[i + 1];
        const BYTE r = pixels[i + 2];
        const BYTE maxChannel = std::max({r, g, b});
        const BYTE minChannel = std::min({r, g, b});
        if (r < 248 || g < 248 || b < 248) {
            ++nonBackground;
            const uint32_t color = (static_cast<uint32_t>(r >> 3) << 10) |
                                   (static_cast<uint32_t>(g >> 3) << 5) |
                                   static_cast<uint32_t>(b >> 3);
            colors.insert(color);
            if (maxChannel - minChannel >= 8) {
                ++chromaticPixels;
                chromaticColors.insert(color);
            }
        }
    }
    if (nonBackground < 100 || colors.size() < 4 ||
        (requireChromaticPixels && (chromaticPixels == 0 || chromaticColors.empty()))) {
        throw std::runtime_error(
            "Rendered PNG is blank or monochrome: non-background pixels=" +
            std::to_string(nonBackground) + ", quantized colors=" + std::to_string(colors.size()) +
            ", chromatic pixels=" + std::to_string(chromaticPixels) +
            ", chromatic colors=" + std::to_string(chromaticColors.size()));
    }
    return {nonBackground, colors.size(), chromaticPixels, chromaticColors.size()};
}

void SavePng(const std::filesystem::path& path, const std::vector<BYTE>& pixels) {
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&factory)),
          "CoCreateInstance(CLSID_WICImagingFactory)");
    ComPtr<IWICStream> stream;
    Check(factory->CreateStream(&stream), "IWICImagingFactory::CreateStream");
    Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE),
          "IWICStream::InitializeFromFilename");
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder),
          "IWICImagingFactory::CreateEncoder(PNG)");
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache),
          "IWICBitmapEncoder::Initialize");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    Check(encoder->CreateNewFrame(&frame, &properties), "IWICBitmapEncoder::CreateNewFrame");
    Check(frame->Initialize(properties.Get()), "IWICBitmapFrameEncode::Initialize");
    Check(frame->SetSize(kCanvasSize, kCanvasSize), "IWICBitmapFrameEncode::SetSize");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    Check(frame->SetPixelFormat(&format), "IWICBitmapFrameEncode::SetPixelFormat");
    if (format != GUID_WICPixelFormat32bppBGRA) {
        throw std::runtime_error("WIC did not accept 32bpp BGRA PNG pixels");
    }
    Check(frame->WritePixels(kCanvasSize, kCanvasSize * 4,
                             static_cast<UINT>(pixels.size()),
                             const_cast<BYTE*>(pixels.data())),
          "IWICBitmapFrameEncode::WritePixels");
    Check(frame->Commit(), "IWICBitmapFrameEncode::Commit");
    Check(encoder->Commit(), "IWICBitmapEncoder::Commit");
}

void LogWindowsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!rtlGetVersion) throw std::runtime_error("Could not resolve RtlGetVersion");
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    const LONG status = rtlGetVersion(&version);
    if (status != 0) throw std::runtime_error("RtlGetVersion failed with status " + std::to_string(status));
    std::cout << "Windows: " << version.dwMajorVersion << '.' << version.dwMinorVersion
              << " build " << version.dwBuildNumber << " (" << Utf8(version.szCSDVersion) << ")\n";
#if defined(_M_X64)
    std::cout << "Process architecture: x64\n";
#elif defined(_M_ARM64)
    std::cout << "Process architecture: arm64\n";
#else
    std::cout << "Process architecture: other\n";
#endif
}

void RenderCase(
    WarpD2D& warp,
    IDWriteFactory4* dwrite,
    IDWriteFontFace* face,
    IDWriteFontFace4* face4,
    IDWriteTextAnalyzer* analyzer,
    const TestCase& test,
    uint32_t emSize,
    const std::filesystem::path& outputDir) {
    ShapedRun shaped = Shape(analyzer, face, test.codepoints, static_cast<FLOAT>(emSize));
    if (shaped.glyphIndices.size() != 1) {
        throw std::runtime_error(std::string(test.id) + " shaped to " +
                                 std::to_string(shaped.glyphIndices.size()) +
                                 " glyphs; expected the one-glyph test sequence");
    }
    const UINT16 glyph = shaped.glyphIndices.front();
    if (glyph == 0) throw std::runtime_error(std::string(test.id) + " shaped to .notdef");
    std::cout << "case=" << test.id << " codepoints=" << CodepointList(test.codepoints)
              << " em=" << emSize << " glyphCount=1 glyphId=" << glyph << '\n';

    DWRITE_GLYPH_IMAGE_DATA imageData{};
    void* imageDataContext = nullptr;
    Check(face4->GetGlyphImageData(glyph, emSize, kPngFormat, &imageData, &imageDataContext),
          "IDWriteFontFace4::GetGlyphImageData(PNG)");
    if (!imageData.imageData || imageData.imageDataSize == 0) {
        if (imageDataContext) face4->ReleaseGlyphImageData(imageDataContext);
        throw std::runtime_error(std::string(test.id) + " has no DirectWrite PNG glyph image data");
    }
    std::cout << "  DWrite PNG imageDataBytes=" << imageData.imageDataSize
              << " pixelsPerEm=" << imageData.pixelsPerEm << '\n';
    face4->ReleaseGlyphImageData(imageDataContext);

    DWRITE_GLYPH_RUN glyphRun{};
    glyphRun.fontFace = face;
    glyphRun.fontEmSize = static_cast<FLOAT>(emSize);
    glyphRun.glyphCount = static_cast<UINT32>(shaped.glyphIndices.size());
    glyphRun.glyphIndices = shaped.glyphIndices.data();
    glyphRun.glyphAdvances = shaped.advances.data();
    glyphRun.glyphOffsets = shaped.offsets.data();
    glyphRun.isSideways = FALSE;
    glyphRun.bidiLevel = 0;

    const float totalAdvance = std::accumulate(shaped.advances.begin(), shaped.advances.end(), 0.0f);
    const D2D1_POINT_2F origin{(kCanvasSize - totalAdvance) / 2.0f, kCanvasSize * 0.72f};
    ComPtr<IDWriteColorGlyphRunEnumerator1> colorRuns;
    Check(dwrite->TranslateColorGlyphRun(
              origin, &glyphRun, nullptr, kPngFormat, DWRITE_MEASURING_MODE_NATURAL,
              nullptr, 0, &colorRuns),
          "IDWriteFactory4::TranslateColorGlyphRun(PNG)");

    RenderTarget target = CreateRenderTarget(warp);
    warp.context->SetTarget(target.bitmap.Get());
    warp.context->BeginDraw();
    warp.context->Clear(D2D1::ColorF(D2D1::ColorF::White));
    UINT32 runCount = 0;
    for (;;) {
        BOOL hasRun = FALSE;
        Check(colorRuns->MoveNext(&hasRun), "IDWriteColorGlyphRunEnumerator1::MoveNext");
        if (!hasRun) break;
        const DWRITE_COLOR_GLYPH_RUN1* colorRun = nullptr;
        Check(colorRuns->GetCurrentRun(&colorRun),
              "IDWriteColorGlyphRunEnumerator1::GetCurrentRun");
        if (!colorRun || colorRun->glyphImageFormat != kPngFormat ||
            colorRun->glyphRun.glyphCount == 0) {
            throw std::runtime_error("DirectWrite returned a non-PNG or empty color glyph run");
        }
        std::cout << "  colorRun=" << runCount << " imageFormat=PNG glyphCount="
                  << colorRun->glyphRun.glyphCount << '\n';
        warp.context4->DrawColorBitmapGlyphRun(
            colorRun->glyphImageFormat,
            D2D1::Point2F(colorRun->baselineOriginX, colorRun->baselineOriginY),
            &colorRun->glyphRun, colorRun->measuringMode,
            D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION_DEFAULT);
        ++runCount;
    }
    if (runCount == 0) throw std::runtime_error("DirectWrite returned no PNG color glyph runs");
    Check(warp.context->EndDraw(), "ID2D1DeviceContext::EndDraw");

    const std::vector<BYTE> pixels = ReadPixels(warp, target);
    const PixelStats pixelStats = CheckColorPixels(pixels, test.expectChromatic && emSize >= 64);
    const std::string filename = std::string(test.id) + "_" + std::to_string(emSize) + ".png";
    SavePng(outputDir / filename, pixels);
    std::cout << "  rendered=" << filename << " nonBackgroundPixels=" << pixelStats.nonBackground
              << " quantizedColors=" << pixelStats.quantizedColors
              << " chromaticPixels=" << pixelStats.chromaticPixels
              << " chromaticColors=" << pixelStats.chromaticColors << '\n';
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 3) {
            std::wcerr << L"Usage: sbix-native-probe.exe <fixture.ttf> <output-directory>\n";
            return 2;
        }
        const std::filesystem::path fontPath(argv[1]);
        const std::filesystem::path outputDir(argv[2]);
        std::filesystem::create_directories(outputDir);
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
        LogWindowsVersion();
        std::cout << "Input font file: " << Utf8(fontPath.wstring()) << '\n';

        ComPtr<IUnknown> factoryUnknown;
        Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory4),
                                  &factoryUnknown),
              "DWriteCreateFactory(IDWriteFactory4)");
        ComPtr<IDWriteFactory4> dwrite;
        Check(factoryUnknown.As(&dwrite), "QueryInterface(IDWriteFactory4)");
        ComPtr<IDWriteFactory> dwriteBase;
        Check(factoryUnknown.As(&dwriteBase), "QueryInterface(IDWriteFactory)");
        ComPtr<IDWriteTextAnalyzer> analyzer;
        Check(dwriteBase->CreateTextAnalyzer(&analyzer), "IDWriteFactory::CreateTextAnalyzer");

        ComPtr<IDWriteFontFile> fontFile;
        Check(dwriteBase->CreateFontFileReference(fontPath.c_str(), nullptr, &fontFile),
              "IDWriteFactory::CreateFontFileReference");
        BOOL supported = FALSE;
        DWRITE_FONT_FILE_TYPE fileType{};
        DWRITE_FONT_FACE_TYPE faceType{};
        UINT32 faceCount = 0;
        Check(fontFile->Analyze(&supported, &fileType, &faceType, &faceCount),
              "IDWriteFontFile::Analyze");
        if (!supported || faceCount == 0) throw std::runtime_error("DirectWrite rejected the font file");
        ComPtr<IDWriteFontFace> face;
        Check(dwriteBase->CreateFontFace(faceType, 1, fontFile.GetAddressOf(), 0,
                                         DWRITE_FONT_SIMULATIONS_NONE, &face),
              "IDWriteFactory::CreateFontFace");
        ComPtr<IDWriteFontFace4> face4;
        Check(face.As(&face4), "QueryInterface(IDWriteFontFace4)");
        DWRITE_FONT_METRICS metrics{};
        face->GetMetrics(&metrics);
        std::cout << "DirectWrite family: " << Utf8(FontFamilyName(face.Get())) << '\n';
        std::cout << "DirectWrite: IDWriteFactory4=available fontFaceType=" << faceType
                  << " glyphCount=" << face->GetGlyphCount()
                  << " unitsPerEm=" << metrics.designUnitsPerEm
                  << " glyphImageFormats=0x" << std::hex << std::uppercase
                  << static_cast<unsigned long>(face4->GetGlyphImageFormats()) << std::dec << '\n';

        WarpD2D warp = CreateWarpD2D();
        std::cout << "Direct2D: ID2D1DeviceContext4=available; D3D driver=WARP; featureLevel=0x"
                  << std::hex << std::uppercase << static_cast<unsigned long>(warp.featureLevel)
                  << std::dec << '\n';
        for (const TestCase& test : kCases) {
            for (uint32_t emSize : kRenderSizes) {
                RenderCase(warp, dwrite.Get(), face.Get(), face4.Get(), analyzer.Get(),
                           test, emSize, outputDir);
            }
        }
        std::cout << "Native DirectWrite/Direct2D sbix probe passed: "
                  << ARRAYSIZE(kCases) << " sequences x " << ARRAYSIZE(kRenderSizes)
                  << " sizes, with explicit file-backed IDWriteFontFace and WARP render targets.\n";
        CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
