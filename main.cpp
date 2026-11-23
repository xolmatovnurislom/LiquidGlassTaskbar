// ============================================================================
//  Liquid Glass Taskbar — Single-file implementation
//  D3D11 + HLSL + DirectComposition + Desktop Duplication + Fallback Capture
// ============================================================================

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shcore.h>
#include <strsafe.h>
#include <wrl/client.h>
#include <memory>
#include <vector>
#include <cstring>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

// ============================================================================
//  HLSL SHADERS (embedded)
// ============================================================================
static const char* g_HlslSource = R"HLSL(
cbuffer Params : register(b0)
{
    float2 g_ScreenOrigin;
    float2 g_ScreenSize;
    float2 g_WindowSize;
    float2 g_GlassOffset;
    float2 g_GlassSize;
    float2 g_TexelSize;
    float2 g_BlurOffset;
    float2 g_MousePos;
    float  g_CornerRadius;
    float  g_Specular;
    float  g_Noise;
    float  g_Chromatic;
    float  g_Material;
    float  g_Time;
    float  g_Opacity;
    float  g_RefractStrength;
    float2 g_Pad;
};

Texture2D    g_Tex   : register(t0);
SamplerState g_Samp  : register(s0);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VS_Main(uint vid : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.uv  = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}

float4 PS_Extract(VSOut i) : SV_Target
{
    float2 screenPx = g_ScreenOrigin + i.uv * g_WindowSize;
    return g_Tex.Sample(g_Samp, screenPx / g_ScreenSize);
}

float4 PS_KawaseDown(VSOut i) : SV_Target
{
    float2 off = g_BlurOffset * g_TexelSize;
    float4 sum = g_Tex.Sample(g_Samp, i.uv);
    sum += g_Tex.Sample(g_Samp, i.uv + float2(-off.x, -off.y));
    sum += g_Tex.Sample(g_Samp, i.uv + float2( off.x, -off.y));
    sum += g_Tex.Sample(g_Samp, i.uv + float2(-off.x,  off.y));
    sum += g_Tex.Sample(g_Samp, i.uv + float2( off.x,  off.y));
    return sum * 0.2;
}

float4 PS_KawaseUp(VSOut i) : SV_Target
{
    float2 off = g_BlurOffset * g_TexelSize;
    float4 sum = 0;
    sum += g_Tex.Sample(g_Samp, i.uv + float2(-off.x * 2.0, 0.0));
    sum += g_Tex.Sample(g_Samp, i.uv + float2(-off.x,  off.y));
    sum += g_Tex.Sample(g_Samp, i.uv + float2( 0.0,     off.y * 2.0));
    sum += g_Tex.Sample(g_Samp, i.uv + float2( off.x,  off.y));
    sum += g_Tex.Sample(g_Samp, i.uv + float2( off.x * 2.0, 0.0));
    sum += g_Tex.Sample(g_Samp, i.uv + float2( off.x, -off.y));
    sum += g_Tex.Sample(g_Samp, i.uv + float2( 0.0,    -off.y * 2.0));
    sum += g_Tex.Sample(g_Samp, i.uv + float2(-off.x, -off.y));
    return sum * 0.125;
}

float sdRoundBox(float2 p, float2 b, float r)
{
    float2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float4 PS_Composite(VSOut i) : SV_Target
{
    float2 winPx     = i.uv * g_WindowSize;
    float2 glassPx   = winPx - g_GlassOffset;
    float2 halfGlass = g_GlassSize * 0.5;
    float2 p         = glassPx - halfGlass;

    float d = sdRoundBox(p, halfGlass, g_CornerRadius);
    if (d > 1.5) return float4(0, 0, 0, 0);

    const float RIM_BAND = 14.0;
    float2 n = float2(0, 0);
    float edgeFactor = saturate(1.0 + d / RIM_BAND);
    if (d > -RIM_BAND)
    {
        float e = 0.5;
        float dx = sdRoundBox(p + float2(e, 0), halfGlass, g_CornerRadius) - d;
        float dy = sdRoundBox(p + float2(0, e), halfGlass, g_CornerRadius) - d;
        n = normalize(float2(dx, dy) + 1e-6);
    }

    float2 sampleUV = i.uv;
    if (d > -RIM_BAND)
    {
        float bendPx = edgeFactor * edgeFactor * 8.0 * g_RefractStrength;
        sampleUV += n * bendPx / g_WindowSize;
    }

    float4 bg;
    if (g_Chromatic > 0.01 && edgeFactor > 0.05)
    {
        float caPx = g_Chromatic * edgeFactor;
        float2 caUV = n * caPx / g_WindowSize;
        float r = g_Tex.Sample(g_Samp, sampleUV + caUV).r;
        float g = g_Tex.Sample(g_Samp, sampleUV).g;
        float b = g_Tex.Sample(g_Samp, sampleUV - caUV).b;
        bg = float4(r, g, b, 1.0);
    }
    else bg = g_Tex.Sample(g_Samp, sampleUV);

    float lum = dot(bg.rgb, float3(0.299, 0.587, 0.114));

    float3 tintMul;
    float  tintAmt;
    if      (g_Material < 0.5) { tintMul = lerp(float3(1.05,1.05,1.10), float3(0.88,0.88,0.95), lum); tintAmt = 0.22; }
    else if (g_Material < 1.5) { tintMul = lerp(float3(1.10,1.10,1.15), float3(0.78,0.78,0.86), lum); tintAmt = 0.45; }
    else                       { tintMul = lerp(float3(0.58,0.58,0.72), float3(0.16,0.16,0.26), lum); tintAmt = 0.72; }
    bg.rgb = lerp(bg.rgb, bg.rgb * tintMul, tintAmt);

    if (g_Specular > 0.01)
    {
        float insideDist = -d;
        float rimW  = 2.2;
        float rim   = saturate(1.0 - abs(insideDist - rimW * 0.5) / (rimW * 0.5));
        float topB  = saturate((-p.y) / halfGlass.y + 0.40);
        float mDist = length(glassPx - g_MousePos);
        float mStr  = saturate(1.0 - mDist / 420.0);
        float spec  = rim * topB * max(0.18, mStr * 0.95) * g_Specular;
        bg.rgb += spec * 0.65;
    }

    if (g_Noise > 0.0)
    {
        float2 nc = glassPx + g_Time * 37.0;
        float n2 = frac(sin(dot(nc, float2(12.9898, 78.233))) * 43758.5453);
        bg.rgb += (n2 - 0.5) * g_Noise;
    }

    float alpha = saturate(-d / 1.5) * g_Opacity;
    return float4(bg.rgb * alpha, alpha);
}
)HLSL";

// ============================================================================
//  GLASS PARAMS
// ============================================================================
struct GlassParams {
    float material        = 1.0f;
    float opacity         = 1.0f;
    float specular        = 1.0f;
    float noise           = 0.015f;
    float chromatic       = 1.0f;
    float refractStrength = 1.0f;
    float cornerRadius    = 18.0f;
};

struct CBData {
    float sox, soy, ssx, ssy;
    float wsx, wsy, gox, goy;
    float gsx, gsy, txw, txh;
    float box, boy, mpx, mpy;
    float corner, spec, noise, chrom;
    float material, time, opacity, refract;
    float padX, padY, pad2X, pad2Y;
};

// ============================================================================
//  GLASS ENGINE (DXGI Desktop Duplication + GDI BitBlt Fallback)
// ============================================================================
class GlassEngine {
public:
    bool Init(HWND hwnd, int winX, int winY, int winW, int winH,
              int glassOX, int glassOY, int glassW, int glassH);
    void Shutdown();
    bool RenderFrame(const GlassParams& p, POINT cursorScreenPx);

private:
    bool CreateDeviceAndSwapchain();
    bool CreateShaders();
    bool CreateBlurChain();
    bool CreateDuplication();
    bool AcquireAndCopyFrame();
    bool CaptureViaGdiFallback();
    void RenderBlurChain();
    void RenderComposite(const GlassParams&, POINT);
    void UpdateCB(const GlassParams&, POINT,
                  float texW, float texH, float bOX, float bOY);
    void DrawFullscreen();
    void Present();

    ComPtr<ID3D11VertexShader>  vs_;
    ComPtr<ID3D11PixelShader>   psE_, psD_, psU_, psC_;
    ComPtr<ID3D11Buffer>        cb_;
    ComPtr<ID3D11SamplerState>  samp_;

    ComPtr<ID3D11Device>           dev_;
    ComPtr<ID3D11DeviceContext>    ctx_;
    ComPtr<IDXGISwapChain1>        swap_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<IDXGIFactory2>          factory_;

    ComPtr<IDCompositionDevice>    dcompDev_;
    ComPtr<IDCompositionTarget>    dcompTgt_;
    ComPtr<IDCompositionVisual>    dcompVis_;

    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D>        capTex_;
    ComPtr<ID3D11ShaderResourceView> capSrv_;
    bool capValid_ = false;
    bool useGdiFallback_ = false;
    ULONGLONG lastGdiTick_ = 0;

    struct Tex {
        ComPtr<ID3D11Texture2D>          tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11RenderTargetView>   rtv;
        UINT w = 0, h = 0;
    };
    std::vector<Tex> down_, up_;

    HWND  hwnd_ = nullptr;
    int   winX_ = 0, winY_ = 0, winW_ = 0, winH_ = 0;
    int   scrW_ = 0, scrH_ = 0;
    int   gOX_ = 0, gOY_ = 0, gW_ = 0, gH_ = 0;
    POINT lastCur_{ -99999, -99999 };
    LARGE_INTEGER qpcFreq_{}, qpcStart_{};
    float time_ = 0.0f;
    bool  presentedOnce_ = false;

    static constexpr int kBlurLevels = 5;
};

#define CHECK(e) do { HRESULT _h = (e); if (FAILED(_h)) { \
    OutputDebugStringA("LGT FAIL: " #e "\n"); return false; } } while (0)

bool GlassEngine::Init(HWND hwnd, int winX, int winY, int winW, int winH,
                       int glassOX, int glassOY, int glassW, int glassH)
{
    hwnd_ = hwnd;
    winX_ = winX; winY_ = winY; winW_ = winW; winH_ = winH;
    gOX_ = glassOX; gOY_ = glassOY; gW_ = glassW; gH_ = glassH;
    scrW_ = GetSystemMetrics(SM_CXSCREEN);
    scrH_ = GetSystemMetrics(SM_CYSCREEN);
    QueryPerformanceFrequency(&qpcFreq_);
    QueryPerformanceCounter(&qpcStart_);

    if (!CreateDeviceAndSwapchain()) return false;
    if (!CreateShaders())            return false;
    if (!CreateBlurChain())          return false;

    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)(scrW_ > 0 ? scrW_ : 1920);
    d.Height = (UINT)(scrH_ > 0 ? scrH_ : 1080);
    d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(dev_->CreateTexture2D(&d, nullptr, &capTex_))) {
        dev_->CreateShaderResourceView(capTex_.Get(), nullptr, &capSrv_);
    }

    if (!CreateDuplication()) {
        useGdiFallback_ = true;
        CaptureViaGdiFallback();
    }
    return true;
}

void GlassEngine::Shutdown()
{
    dup_.Reset(); capSrv_.Reset(); capTex_.Reset();
    down_.clear(); up_.clear();
    if (dcompDev_) dcompDev_->Commit();
    dcompVis_.Reset(); dcompTgt_.Reset(); dcompDev_.Reset();
    rtv_.Reset(); swap_.Reset();
    psC_.Reset(); psU_.Reset(); psD_.Reset(); psE_.Reset();
    vs_.Reset(); cb_.Reset(); samp_.Reset();
    ctx_.Reset(); dev_.Reset(); factory_.Reset();
}

bool GlassEngine::CreateDeviceAndSwapchain()
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                            lv, _countof(lv), D3D11_SDK_VERSION,
                            &dev_, &got, &ctx_));

    ComPtr<IDXGIDevice> dxgiDev;
    CHECK(dev_.As(&dxgiDev));
    ComPtr<IDXGIAdapter> ad;
    CHECK(dxgiDev->GetAdapter(&ad));
    CHECK(ad->GetParent(IID_PPV_ARGS(&factory_)));

    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width = (UINT)winW_; sc.Height = (UINT)winH_;
    sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.Scaling = DXGI_SCALING_STRETCH;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    CHECK(factory_->CreateSwapChainForComposition(dev_.Get(), &sc, nullptr, &swap_));

    ComPtr<ID3D11Texture2D> back;
    CHECK(swap_->GetBuffer(0, IID_PPV_ARGS(&back)));
    CHECK(dev_->CreateRenderTargetView(back.Get(), nullptr, &rtv_));

    CHECK(DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&dcompDev_)));
    CHECK(dcompDev_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTgt_));
    CHECK(dcompDev_->CreateVisual(&dcompVis_));
    CHECK(dcompVis_->SetContent(swap_.Get()));
    CHECK(dcompTgt_->SetRoot(dcompVis_.Get()));
    CHECK(dcompDev_->Commit());
    return true;
}

static ComPtr<ID3DBlob> Comp(const char* entry, const char* target)
{
    ComPtr<ID3DBlob> code, err;
    UINT f = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT hr = D3DCompile(g_HlslSource, strlen(g_HlslSource), "LGT",
                            nullptr, nullptr, entry, target, f, 0, &code, &err);
    if (FAILED(hr)) {
        if (err) OutputDebugStringA((const char*)err->GetBufferPointer());
        return nullptr;
    }
    return code;
}

bool GlassEngine::CreateShaders()
{
    auto a = Comp("VS_Main", "vs_5_0");
    auto b = Comp("PS_Extract", "ps_5_0");
    auto c = Comp("PS_KawaseDown", "ps_5_0");
    auto d = Comp("PS_KawaseUp", "ps_5_0");
    auto e = Comp("PS_Composite", "ps_5_0");
    if (!a || !b || !c || !d || !e) return false;

    CHECK(dev_->CreateVertexShader(a->GetBufferPointer(), a->GetBufferSize(), nullptr, &vs_));
    CHECK(dev_->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psE_));
    CHECK(dev_->CreatePixelShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &psD_));
    CHECK(dev_->CreatePixelShader(d->GetBufferPointer(), d->GetBufferSize(), nullptr, &psU_));
    CHECK(dev_->CreatePixelShader(e->GetBufferPointer(), e->GetBufferSize(), nullptr, &psC_));

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(CBData);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    CHECK(dev_->CreateBuffer(&cbd, nullptr, &cb_));

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    CHECK(dev_->CreateSamplerState(&sd, &samp_));
    return true;
}

static bool MakeTex(ID3D11Device* dev, UINT w, UINT h,
                    ComPtr<ID3D11Texture2D>& t,
                    ComPtr<ID3D11ShaderResourceView>& s,
                    ComPtr<ID3D11RenderTargetView>& r)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w ? w : 1; d.Height = h ? h : 1;
    d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &t))) return false;
    if (FAILED(dev->CreateShaderResourceView(t.Get(), nullptr, &s))) return false;
    if (FAILED(dev->CreateRenderTargetView(t.Get(), nullptr, &r))) return false;
    return true;
}

bool GlassEngine::CreateBlurChain()
{
    down_.clear(); up_.clear();
    down_.resize(kBlurLevels + 1);
    up_.resize(kBlurLevels);

    UINT w = winW_, h = winH_;
    for (int i = 0; i <= kBlurLevels; i++) {
        if (!MakeTex(dev_.Get(), w, h, down_[i].tex, down_[i].srv, down_[i].rtv))
            return false;
        down_[i].w = w; down_[i].h = h;
        w = w / 2 < 1 ? 1 : w / 2;
        h = h / 2 < 1 ? 1 : h / 2;
    }
    for (int i = 0; i < kBlurLevels; i++) {
        UINT tw = down_[kBlurLevels - 1 - i].w;
        UINT th = down_[kBlurLevels - 1 - i].h;
        if (!MakeTex(dev_.Get(), tw, th, up_[i].tex, up_[i].srv, up_[i].rtv))
            return false;
        up_[i].w = tw; up_[i].h = th;
    }
    return true;
}

bool GlassEngine::CreateDuplication()
{
    dup_.Reset();
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(dev_.As(&dxgiDev))) return false;
    ComPtr<IDXGIAdapter> ad;
    if (FAILED(dxgiDev->GetAdapter(&ad))) return false;
    ComPtr<IDXGIOutput> out;
    if (FAILED(ad->EnumOutputs(0, &out))) return false;
    ComPtr<IDXGIOutput1> out1;
    if (FAILED(out.As(&out1))) return false;
    if (FAILED(out1->DuplicateOutput(dev_.Get(), &dup_))) return false;
    return true;
}

bool GlassEngine::CaptureViaGdiFallback()
{
    if (!capTex_) return false;
    int capW = winW_ > 0 ? winW_ : 1;
    int capH = winH_ > 0 ? winH_ : 1;

    HDC hdcScr = GetDC(nullptr);
    if (!hdcScr) return false;
    HDC hdcMem = CreateCompatibleDC(hdcScr);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = capW;
    bmi.bmiHeader.biHeight = -capH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdcScr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hBmp || !bits) {
        if (hBmp) DeleteObject(hBmp);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScr);
        return false;
    }

    HGDIOBJ oldObj = SelectObject(hdcMem, hBmp);
    BitBlt(hdcMem, 0, 0, capW, capH, hdcScr, winX_, winY_, SRCCOPY);

    D3D11_BOX box{};
    box.left = (UINT)(winX_ < 0 ? 0 : winX_);
    box.top = (UINT)(winY_ < 0 ? 0 : winY_);
    box.right = box.left + (UINT)capW;
    box.bottom = box.top + (UINT)capH;
    box.front = 0;
    box.back = 1;

    if (box.right <= (UINT)scrW_ && box.bottom <= (UINT)scrH_) {
        ctx_->UpdateSubresource(capTex_.Get(), 0, &box, bits, (UINT)(capW * 4), 0);
        capValid_ = true;
    }

    SelectObject(hdcMem, oldObj);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScr);
    return capValid_;
}

bool GlassEngine::AcquireAndCopyFrame()
{
    if (!useGdiFallback_) {
        if (!dup_) {
            if (!CreateDuplication()) {
                useGdiFallback_ = true;
            }
        }
        if (dup_ && !useGdiFallback_) {
            DXGI_OUTDUPL_FRAME_INFO info{};
            ComPtr<IDXGIResource> res;
            HRESULT hr = dup_->AcquireNextFrame(0, &info, &res);
            if (SUCCEEDED(hr)) {
                ComPtr<ID3D11Texture2D> src;
                if (SUCCEEDED(res.As(&src))) {
                    ctx_->CopyResource(capTex_.Get(), src.Get());
                    capValid_ = true;
                }
                dup_->ReleaseFrame();
                return true;
            }
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                dup_.Reset();
                if (!CreateDuplication()) useGdiFallback_ = true;
            } else if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
                useGdiFallback_ = true;
            }
        }
    }

    if (useGdiFallback_) {
        ULONGLONG nowTick = GetTickCount64();
        if (!capValid_ || (nowTick - lastGdiTick_) >= 66) {
            lastGdiTick_ = nowTick;
            return CaptureViaGdiFallback();
        }
    }
    return false;
}

void GlassEngine::UpdateCB(const GlassParams& p, POINT cur,
                           float texW, float texH, float bOX, float bOY)
{
    CBData d{};
    d.sox = 0.0f; d.soy = 0.0f;
    d.ssx = (float)scrW_; d.ssy = (float)scrH_;
    d.wsx = (float)winW_; d.wsy = (float)winH_;
    d.gox = (float)gOX_;  d.goy = (float)gOY_;
    d.gsx = (float)gW_;   d.gsy = (float)gH_;
    d.txw = texW; d.txh = texH;
    d.box = bOX;  d.boy = bOY;
    d.mpx = (float)(cur.x - winX_ - gOX_);
    d.mpy = (float)(cur.y - winY_ - gOY_);
    d.corner    = p.cornerRadius;
    d.spec      = p.specular;
    d.noise     = p.noise;
    d.chrom     = p.chromatic;
    d.material  = p.material;
    d.time      = time_;
    d.opacity   = p.opacity;
    d.refract   = p.refractStrength;

    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &d, sizeof(d));
        ctx_->Unmap(cb_.Get(), 0);
    }
    ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
}

void GlassEngine::DrawFullscreen()
{
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(nullptr);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->Draw(3, 0);
}

void GlassEngine::RenderBlurChain()
{
    GlassParams dummy;
    POINT zero{ 0, 0 };
    ctx_->PSSetSamplers(0, 1, samp_.GetAddressOf());

    {
        ID3D11RenderTargetView* r = down_[0].rtv.Get();
        ctx_->OMSetRenderTargets(1, &r, nullptr);
        ctx_->PSSetShader(psE_.Get(), nullptr, 0);
        ctx_->PSSetShaderResources(0, 1, capSrv_.GetAddressOf());
        UpdateCB(dummy, zero, 1.0f / scrW_, 1.0f / scrH_, 0.0f, 0.0f);
        DrawFullscreen();
    }
    for (int i = 0; i < kBlurLevels; i++) {
        ID3D11RenderTargetView* r = down_[i + 1].rtv.Get();
        ctx_->OMSetRenderTargets(1, &r, nullptr);
        ctx_->PSSetShader(psD_.Get(), nullptr, 0);
        ctx_->PSSetShaderResources(0, 1, down_[i].srv.GetAddressOf());
        UpdateCB(dummy, zero, 1.0f / down_[i].w, 1.0f / down_[i].h, 1.0f, 1.0f);
        DrawFullscreen();
    }
    for (int i = 0; i < kBlurLevels; i++) {
        int srcIdx = kBlurLevels - i;
        ID3D11RenderTargetView* r = up_[i].rtv.Get();
        ctx_->OMSetRenderTargets(1, &r, nullptr);
        ctx_->PSSetShader(psU_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* src = (i == 0) ? down_[srcIdx].srv.Get()
                                                 : up_[i - 1].srv.Get();
        UINT sw = (i == 0) ? down_[srcIdx].w : up_[i - 1].w;
        UINT sh = (i == 0) ? down_[srcIdx].h : up_[i - 1].h;
        ctx_->PSSetShaderResources(0, 1, &src);
        UpdateCB(dummy, zero, 1.0f / sw, 1.0f / sh, 1.0f, 1.0f);
        DrawFullscreen();
    }
}

void GlassEngine::RenderComposite(const GlassParams& p, POINT cur)
{
    ID3D11RenderTargetView* r = rtv_.Get();
    ctx_->OMSetRenderTargets(1, &r, nullptr);
    ctx_->PSSetShader(psC_.Get(), nullptr, 0);
    ctx_->PSSetShaderResources(0, 1, up_[kBlurLevels - 1].srv.GetAddressOf());
    UpdateCB(p, cur, 1.0f / winW_, 1.0f / winH_, 0.0f, 0.0f);
    DrawFullscreen();
}

void GlassEngine::Present()
{
    swap_->Present(1, 0);
}

bool GlassEngine::RenderFrame(const GlassParams& p, POINT cur)
{
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    time_ = (float)(now.QuadPart - qpcStart_.QuadPart) / (float)qpcFreq_.QuadPart;

    bool mouseMoved = (cur.x != lastCur_.x || cur.y != lastCur_.y);
    lastCur_ = cur;

    bool gotNew = AcquireAndCopyFrame();
    if (gotNew) RenderBlurChain();

    if (!capValid_) return false;
    if (!gotNew && !mouseMoved && presentedOnce_) return false;

    RenderComposite(p, cur);
    Present();
    presentedOnce_ = true;
    return true;
}

// ============================================================================
//  TASKBAR MANAGER
// ============================================================================
class TaskbarManager {
public:
    void Install();
    void Restore();
    void Reapply();
    UINT TaskbarCreatedMessage() const { return taskbarCreatedMsg_; }
    static bool IsFullscreenAppActive();

private:
    void ApplyTransparencyToAll();
    void ApplyTransparency(HWND hwnd);
    void RestoreWindow(HWND hwnd);

    struct SavedState { HWND hwnd; DWORD exStyle; DWORD style; };
    SavedState saved_[8]{};
    int  savedCount_ = 0;
    UINT taskbarCreatedMsg_ = 0;
    bool installed_ = false;
};

typedef enum {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4
} ACCENT_STATE;

typedef struct { ACCENT_STATE AccentState; DWORD AccentFlags; DWORD GradientColor; DWORD AnimationId; } ACCENT_POLICY;
typedef struct { DWORD Attribute; PVOID pData; SIZE_T cbData; } WINCOMPATTRDATA;

typedef BOOL(WINAPI* pSetWCA)(HWND, WINCOMPATTRDATA*);

static pSetWCA GetSetWCA()
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    return (pSetWCA)GetProcAddress(u, "SetWindowCompositionAttribute");
}

void TaskbarManager::Install()
{
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    ApplyTransparencyToAll();
    installed_ = true;
}

void TaskbarManager::Reapply()
{
    if (!installed_) return;
    ApplyTransparencyToAll();
}

void TaskbarManager::Restore()
{
    for (int i = 0; i < savedCount_; i++) RestoreWindow(saved_[i].hwnd);
    savedCount_ = 0;
    installed_ = false;
}

static void EnumTaskbars(std::vector<HWND>& out)
{
    out.clear();
    HWND t = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (t) out.push_back(t);

    struct Ctx { std::vector<HWND>* v; };
    Ctx c{ &out };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        wchar_t cls[64]{};
        GetClassNameW(h, cls, 64);
        if (wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0)
            ((Ctx*)lp)->v->push_back(h);
        return TRUE;
    }, (LPARAM)&c);
}

void TaskbarManager::ApplyTransparencyToAll()
{
    std::vector<HWND> bars;
    EnumTaskbars(bars);

    for (HWND h : bars) {
        bool seen = false;
        for (int i = 0; i < savedCount_; i++)
            if (saved_[i].hwnd == h) { seen = true; break; }
        if (!seen && savedCount_ < 8) {
            saved_[savedCount_].hwnd = h;
            saved_[savedCount_].exStyle = (DWORD)GetWindowLongPtrW(h, GWL_EXSTYLE);
            saved_[savedCount_].style   = (DWORD)GetWindowLongPtrW(h, GWL_STYLE);
            savedCount_++;
        }
        ApplyTransparency(h);
        EnumChildWindows(h, [](HWND child, LPARAM) -> BOOL {
            auto f = GetSetWCA();
            if (f) {
                ACCENT_POLICY ap{};
                ap.AccentState   = ACCENT_ENABLE_TRANSPARENTGRADIENT;
                ap.AccentFlags   = 2;
                ap.GradientColor = 0x00000000;
                WINCOMPATTRDATA d{};
                d.Attribute = 19;
                d.pData = &ap;
                d.cbData = sizeof(ap);
                f(child, &d);
            }
            return TRUE;
        }, 0);
    }
}

void TaskbarManager::ApplyTransparency(HWND hwnd)
{
    auto f = GetSetWCA();
    if (!f) return;
    ACCENT_POLICY ap{};
    ap.AccentState   = ACCENT_ENABLE_TRANSPARENTGRADIENT;
    ap.AccentFlags   = 2;
    ap.GradientColor = 0x00000000;
    WINCOMPATTRDATA d{};
    d.Attribute = 19;
    d.pData = &ap;
    d.cbData = sizeof(ap);
    f(hwnd, &d);
}

void TaskbarManager::RestoreWindow(HWND hwnd)
{
    if (!IsWindow(hwnd)) return;
    auto f = GetSetWCA();
    if (f) {
        ACCENT_POLICY ap{};
        ap.AccentState = ACCENT_DISABLED;
        WINCOMPATTRDATA d{};
        d.Attribute = 19;
        d.pData = &ap;
        d.cbData = sizeof(ap);
        f(hwnd, &d);
    }
    EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
        auto f = GetSetWCA();
        if (f) {
            ACCENT_POLICY ap{};
            ap.AccentState = ACCENT_DISABLED;
            WINCOMPATTRDATA d{};
            d.Attribute = 19;
            d.pData = &ap;
            d.cbData = sizeof(ap);
            f(child, &d);
        }
        return TRUE;
    }, 0);
}

bool TaskbarManager::IsFullscreenAppActive()
{
    QUERY_USER_NOTIFICATION_STATE qs{};
    if (SUCCEEDED(SHQueryUserNotificationState(&qs))) {
        if (qs == QUNS_BUSY || qs == QUNS_RUNNING_D3D_FULL_SCREEN
            || qs == QUNS_PRESENTATION_MODE) return true;
    }
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    wchar_t cls[64]{};
    GetClassNameW(fg, cls, 64);
    if (wcscmp(cls, L"Shell_TrayWnd") == 0
        || wcscmp(cls, L"Progman") == 0
        || wcscmp(cls, L"WorkerW") == 0) return false;
    RECT r{}; GetWindowRect(fg, &r);
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    return (r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top
            && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom);
}

// ============================================================================
//  TRAY ICON
// ============================================================================
enum class TrayCmd {
    OpenWindow = 1000,
    ToggleOnOff = 1005,
    StylePill = 1001, StyleFullWidth = 1002,
    MatClear = 1010, MatFrost = 1011, MatObsidian = 1012,
    ToggleGlint = 1020, Autostart = 1030, Quit = 1040
};

class TrayIcon {
public:
    bool Install(HWND hwnd, UINT wmApp);
    void Remove();
    void ShowMenu(bool effectEnabled);
private:
    HWND  hwnd_ = nullptr;
    UINT  wmApp_ = 0;
    NOTIFYICONDATAW nid_{};
};

bool TrayIcon::Install(HWND hwnd, UINT wmApp)
{
    hwnd_ = hwnd; wmApp_ = wmApp;
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd;
    nid_.uID = 1;
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid_.uCallbackMessage = wmApp;
    nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    StringCchCopyW(nid_.szTip, _countof(nid_.szTip), L"Liquid Glass Taskbar");
    return Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
}

void TrayIcon::Remove() { Shell_NotifyIconW(NIM_DELETE, &nid_); }

void TrayIcon::ShowMenu(bool effectEnabled)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, (UINT_PTR)TrayCmd::OpenWindow, L"Open Control Window");
    AppendMenuW(m, MF_STRING, (UINT_PTR)TrayCmd::ToggleOnOff,
                effectEnabled ? L"Turn OFF Effect" : L"Turn ON Effect");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    HMENU style = CreatePopupMenu();
    AppendMenuW(style, MF_STRING, (UINT_PTR)TrayCmd::StylePill,     L"Floating Pill (Apple Dock)");
    AppendMenuW(style, MF_STRING, (UINT_PTR)TrayCmd::StyleFullWidth, L"Full-Width Capsule");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)style, L"Style");

    HMENU mat = CreatePopupMenu();
    AppendMenuW(mat, MF_STRING, (UINT_PTR)TrayCmd::MatClear,     L"Clear Liquid");
    AppendMenuW(mat, MF_STRING, (UINT_PTR)TrayCmd::MatFrost,     L"Frosted Crystal");
    AppendMenuW(mat, MF_STRING, (UINT_PTR)TrayCmd::MatObsidian,  L"Obsidian Glass");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)mat, L"Material");

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, (UINT_PTR)TrayCmd::ToggleGlint, L"Interactive Light Glint");
    AppendMenuW(m, MF_STRING, (UINT_PTR)TrayCmd::Autostart,   L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, (UINT_PTR)TrayCmd::Quit,        L"Quit");

    SetForegroundWindow(hwnd_);
    POINT p; GetCursorPos(&p);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   p.x, p.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
}

// ============================================================================
//  APP STATE + CONTROL WINDOW + MAIN
// ============================================================================
static const wchar_t* kClassName = L"LiquidGlassTaskbarWnd";
static const wchar_t* kCtrlClassName = L"LiquidGlassControlWnd";
static const UINT WM_TRAY = WM_APP + 1;
static constexpr UINT ID_BTN_TOGGLE = 2001;

struct AppState {
    HWND hwnd = nullptr;
    HWND hwndCtrl = nullptr;
    HWND hwndBtnToggle = nullptr;
    HWND hwndStatusLabel = nullptr;
    HINSTANCE hInst = nullptr;
    std::unique_ptr<GlassEngine> engine;
    TaskbarManager taskbar;
    TrayIcon tray;
    GlassParams params;
    bool effectEnabled = true;
    bool fullWidth = false;
    bool glintOn = true;
    int  scrW = 0, scrH = 0;
    int  winX = 0, winY = 0, winW = 0, winH = 0;
    int  gOX = 0, gOY = 0, gW = 0, gH = 0;
};
static AppState g;

static void UpdateControlUI()
{
    if (g.hwndBtnToggle && IsWindow(g.hwndBtnToggle)) {
        SetWindowTextW(g.hwndBtnToggle, g.effectEnabled ? L"TURN OFF" : L"TURN ON");
    }
    if (g.hwndStatusLabel && IsWindow(g.hwndStatusLabel)) {
        SetWindowTextW(g.hwndStatusLabel,
                       g.effectEnabled ? L"Effect Status: ON" : L"Effect Status: OFF");
    }
}

static void SetEffectEnabled(bool enabled)
{
    g.effectEnabled = enabled;
    if (g.hwnd && IsWindow(g.hwnd)) {
        ShowWindow(g.hwnd, g.effectEnabled ? SW_SHOWNA : SW_HIDE);
    }
    if (g.effectEnabled) {
        g.taskbar.Install();
    } else {
        g.taskbar.Restore();
    }
    UpdateControlUI();
}

static LRESULT CALLBACK CtrlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        g.hwndStatusLabel = CreateWindowExW(
            0, L"STATIC", L"Effect Status: ON",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            20, 22, 260, 24,
            hwnd, nullptr, g.hInst, nullptr);
        SendMessageW(g.hwndStatusLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        g.hwndBtnToggle = CreateWindowExW(
            0, L"BUTTON", L"TURN OFF",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            75, 62, 150, 40,
            hwnd, (HMENU)ID_BTN_TOGGLE, g.hInst, nullptr);
        SendMessageW(g.hwndBtnToggle, WM_SETFONT, (WPARAM)hFont, TRUE);

        UpdateControlUI();
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == ID_BTN_TOGGLE) {
            SetEffectEnabled(!g.effectEnabled);
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowControlWindow()
{
    if (g.hwndCtrl && IsWindow(g.hwndCtrl)) {
        ShowWindow(g.hwndCtrl, SW_SHOWNORMAL);
        SetForegroundWindow(g.hwndCtrl);
        UpdateControlUI();
        return;
    }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = CtrlWndProc;
    wc.hInstance = g.hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kCtrlClassName;
    RegisterClassExW(&wc);

    int w = 315, h = 165;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    g.hwndCtrl = CreateWindowExW(
        WS_EX_TOPMOST,
        kCtrlClassName,
        L"Liquid Glass Control",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, w, h,
        nullptr, nullptr, g.hInst, nullptr);

    ShowWindow(g.hwndCtrl, SW_SHOW);
    UpdateWindow(g.hwndCtrl);
}

static bool GetPrimaryTaskbarRect(RECT& r)
{
    HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tb) return false;
    return GetWindowRect(tb, &r) != FALSE;
}

static void ComputeGeometry()
{
    g.scrW = GetSystemMetrics(SM_CXSCREEN);
    g.scrH = GetSystemMetrics(SM_CYSCREEN);

    RECT tb{};
    if (!GetPrimaryTaskbarRect(tb)) {
        tb.left = 0; tb.right = g.scrW;
        tb.top  = g.scrH - 48; tb.bottom = g.scrH;
    }

    int marginH = g.fullWidth ? 0 : 12;
    int marginT = 6;
    int marginB = 6;
    int extra   = 4;

    g.winX = tb.left;
    g.winY = tb.top - extra;
    g.winW = tb.right - tb.left;
    g.winH = tb.bottom - tb.top + extra + 4;

    g.gOX = marginH;
    g.gOY = marginT + extra;
    g.gW  = g.winW - marginH * 2;
    g.gH  = g.winH - marginT - marginB - extra;
}

static void ToggleAutostart(bool on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;

    if (on) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        RegSetValueExW(k, L"LiquidGlassTaskbar", 0, REG_SZ,
                       (const BYTE*)path,
                       (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"LiquidGlassTaskbar");
    }
    RegCloseKey(k);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g.taskbar.TaskbarCreatedMessage() && msg != 0) {
        if (g.effectEnabled) g.taskbar.Reapply();
        ComputeGeometry();
        SetWindowPos(hwnd, HWND_TOPMOST, g.winX, g.winY, g.winW, g.winH,
                     SWP_NOACTIVATE | (g.effectEnabled ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP) {
            g.tray.ShowMenu(g.effectEnabled);
        } else if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
            ShowControlWindow();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case (UINT)TrayCmd::OpenWindow:
            ShowControlWindow();
            return 0;
        case (UINT)TrayCmd::ToggleOnOff:
            SetEffectEnabled(!g.effectEnabled);
            return 0;
        case (UINT)TrayCmd::StylePill:
            g.fullWidth = false;
            ComputeGeometry();
            SetWindowPos(hwnd, HWND_TOPMOST, g.winX, g.winY, g.winW, g.winH,
                         SWP_NOACTIVATE | (g.effectEnabled ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
            return 0;
        case (UINT)TrayCmd::StyleFullWidth:
            g.fullWidth = true;
            ComputeGeometry();
            SetWindowPos(hwnd, HWND_TOPMOST, g.winX, g.winY, g.winW, g.winH,
                         SWP_NOACTIVATE | (g.effectEnabled ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
            return 0;
        case (UINT)TrayCmd::MatClear:    g.params.material = 0.0f; return 0;
        case (UINT)TrayCmd::MatFrost:    g.params.material = 1.0f; return 0;
        case (UINT)TrayCmd::MatObsidian: g.params.material = 2.0f; return 0;
        case (UINT)TrayCmd::ToggleGlint:
            g.glintOn = !g.glintOn;
            g.params.specular = g.glintOn ? 1.0f : 0.0f;
            return 0;
        case (UINT)TrayCmd::Autostart:   ToggleAutostart(true); return 0;
        case (UINT)TrayCmd::Quit:        PostQuitMessage(0);    return 0;
        }
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        ComputeGeometry();
        SetWindowPos(hwnd, HWND_TOPMOST, g.winX, g.winY, g.winW, g.winH,
                     SWP_NOACTIVATE | (g.effectEnabled ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        return 0;

    case WM_CLOSE:  PostQuitMessage(0); return 0;
    case WM_DESTROY: return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    g.hInst = hInst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    ComputeGeometry();

    g.hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
        WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP,
        kClassName, L"Liquid Glass", WS_POPUP,
        g.winX, g.winY, g.winW, g.winH,
        nullptr, nullptr, hInst, nullptr);
    if (!g.hwnd) return 1;

    g.engine = std::make_unique<GlassEngine>();
    if (!g.engine->Init(g.hwnd, g.winX, g.winY, g.winW, g.winH,
                        g.gOX, g.gOY, g.gW, g.gH)) {
        MessageBoxW(nullptr, L"D3D11/DComp init failed", L"Error", MB_ICONERROR);
        return 1;
    }

    g.taskbar.Install();
    g.tray.Install(g.hwnd, WM_TRAY);
    ShowWindow(g.hwnd, SW_SHOWNA);
    ShowControlWindow();

    MSG msg{};
    while (true) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g.tray.Remove();
                g.taskbar.Restore();
                if (g.engine) g.engine->Shutdown();
                return (int)msg.wParam;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (g.effectEnabled && !TaskbarManager::IsFullscreenAppActive()) {
            POINT cur{};
            GetCursorPos(&cur);
            g.engine->RenderFrame(g.params, cur);
        } else {
            Sleep(30);
        }
        Sleep(10);
    }
}
