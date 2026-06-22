#include "PostProcess.h"
#include "Log.h"

#include <d3dcompiler.h>

#include <cmath>
#include <cstring>

namespace vf::post {

Grade g_grade;
bool g_sharpenEnabled = false;
float g_sharpness = 0.4f;
bool g_lutEnabled = false;
float g_lutIntensity = 1.0f;
Ssao g_ssao;
Panini g_panini;
int g_debugView = 0;
float g_debugGamma = 64.0f;

namespace {

// Color grading followed by AMD FidelityFX Contrast Adaptive Sharpening, in one pass.
// Grading runs on the (optionally sharpened) color. All toggles are passed as 0/1 floats so
// the same compiled shader serves every combination.
const char kShader[] = R"hlsl(
Texture2D srcTex : register(t0);
Texture3D lutTex : register(t1);
Texture2D depthTex : register(t2);
SamplerState lutSampler : register(s0);

cbuffer Params : register(b0)
{
    float sharpness;
    float exposure;
    float contrast;
    float saturation;
    float vibrance;
    float temperature;
    float tint;
    float filmic;
    float gradeEnabled;
    float sharpenEnabled;
    float lutEnabled;
    float lutIntensity;
    float lutSize;
    float debugView;      // 0 = off, 1 = scene depth, 2 = ambient occlusion
    float debugGamma;     // shaping power for the depth view
    float depthAvailable;

    float ssaoEnabled;
    float ssaoIntensity;
    float ssaoRadius;     // world units
    float ssaoBias;

    float nearZ;
    float farZ;
    float projScaleX;     // tan(fovX/2)
    float projScaleY;     // tan(fovY/2)

    float giEnabled;
    float giIntensity;
    float paniniEnabled;
    float paniniStrength;

    float paniniZoom;
    float3 pad2;
};

struct VSOut { float4 pos : SV_Position; };

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float3 Fetch(int2 p, int2 size)
{
    return srcTex.Load(int3(clamp(p, int2(0, 0), size - 1), 0)).rgb;
}

float3 FetchBilinear(float2 pix, int2 size)
{
    float2 uv = (pix + 0.5) / float2(size);
    return srcTex.SampleLevel(lutSampler, saturate(uv), 0.0).rgb;
}

// ---- Panini projection -----------------------------------------------------------------
// Rectilinear projection stretches the edges of a wide field of view; Panini trades that
// stretch for gently curved horizontal lines while keeping vertical lines straight.
// This is a screen-space remap: for each output pixel it solves for the view angle the
// Panini projection would put there, then returns where that angle lands in the game's
// rectilinear frame. strength 0 reproduces the original image exactly.
//
// Because Panini pulls the edges inward, the outermost pixels come from *outside* the
// game's frustum. The engine's FOV therefore has to be widened to supply them, otherwise
// the border smears. VisualForge computes and applies that FOV compensation.
float2 PaniniSource(int2 p, int2 size)
{
    float2 ndc = ((float2(p) + 0.5) / float2(size)) * 2.0 - 1.0;

    // Work in tangent units so the distortion is tied to real view angles, not pixels.
    float2 t = ndc * float2(projScaleX, projScaleY) * paniniZoom;

    float d = paniniStrength;
    float dd = d + 1.0;
    float x = t.x;

    // Solve (d+1)*sin(theta) / (d + cos(theta)) = x for cos(theta).
    float A = x * x + dd * dd;
    float B = 2.0 * x * x * d;
    float C = x * x * d * d - dd * dd;
    float disc = max(B * B - 4.0 * A * C, 0.0);
    float c = (-B + sqrt(disc)) / (2.0 * A);
    c = clamp(c, 1e-4, 1.0);

    float s = sqrt(saturate(1.0 - c * c)) * (x < 0.0 ? -1.0 : 1.0);
    float S = dd / (d + c);

    float uR = s / c;              // tan(theta)
    float vR = (t.y / S) / c;

    float2 outNdc = float2(uR / max(projScaleX, 1e-6), vR / max(projScaleY, 1e-6));
    return (outNdc * 0.5 + 0.5) * float2(size) - 0.5;
}

// ---- Screen-space ambient occlusion / global illumination -------------------------------
// Depth-only: view-space positions are rebuilt from the captured depth buffer and normals
// are derived from its derivatives, so no G-buffer access is required. This approximates
// contact shadowing and one bounce of local light from what is on screen; it cannot know
// about anything off screen or behind geometry.

#define SSAO_SAMPLES 16
static const float kGoldenAngle = 2.39996323;

float LinearZ(float d)
{
    return nearZ * farZ / max(farZ - d * (farZ - nearZ), 1e-6);
}

float3 ViewPosAt(int2 pix, int2 size)
{
    int2 c = clamp(pix, int2(0, 0), size - 1);
    float d = depthTex.Load(int3(c, 0)).r;
    float z = LinearZ(d);
    float2 uv = (float2(c) + 0.5) / float2(size);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return float3(ndc * float2(projScaleX, projScaleY) * z, z);
}

// Interleaved gradient noise — cheap per-pixel rotation so the sample pattern does not band.
float IGN(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Picks the nearer neighbour on each axis so normals stay sharp across depth discontinuities.
float3 NormalFromDepth(int2 pix, int2 size, float3 p)
{
    float3 pr = ViewPosAt(pix + int2(1, 0), size);
    float3 pl = ViewPosAt(pix - int2(1, 0), size);
    float3 pu = ViewPosAt(pix + int2(0, 1), size);
    float3 pd = ViewPosAt(pix - int2(0, 1), size);

    float3 dx = (abs(pr.z - p.z) < abs(p.z - pl.z)) ? (pr - p) : (p - pl);
    float3 dy = (abs(pu.z - p.z) < abs(p.z - pd.z)) ? (pu - p) : (p - pd);

    float3 n = normalize(cross(dx, dy));
    // View-space normals must face the camera (origin); flip if the cross product came out
    // the other way for this handedness.
    if (dot(n, -normalize(p)) < 0.0)
        n = -n;
    return n;
}

// Returns occlusion in .a (1 = unoccluded) and gathered bounce light in .rgb.
float4 ComputeSSAO(int2 pix, int2 size)
{
    float rawDepth = depthTex.Load(int3(clamp(pix, int2(0, 0), size - 1), 0)).r;
    if (rawDepth >= 0.99999)             // sky / cleared — nothing to occlude
        return float4(0, 0, 0, 1);

    float3 p = ViewPosAt(pix, size);
    float3 n = NormalFromDepth(pix, size, p);

    // World radius projected to pixels, so the effect keeps a constant world-space size.
    float radiusPix = (ssaoRadius / max(p.z * projScaleY, 1e-4)) * (float(size.y) * 0.5);
    radiusPix = clamp(radiusPix, 2.0, 192.0);

    float noise = IGN(float2(pix));
    float occlusion = 0.0;
    float3 bounce = 0.0;
    float r2 = ssaoRadius * ssaoRadius;

    [loop]
    for (int i = 0; i < SSAO_SAMPLES; ++i) {
        float ang = (float(i) + noise) * kGoldenAngle;
        float rad = sqrt((float(i) + 0.5) / float(SSAO_SAMPLES)) * radiusPix;
        int2 sp = pix + int2(round(cos(ang) * rad), round(sin(ang) * rad));

        float3 q = ViewPosAt(sp, size);
        float3 v = q - p;
        float dist2 = dot(v, v);
        float nDotV = dot(n, v) * rsqrt(max(dist2, 1e-6));

        float falloff = saturate(1.0 - dist2 / max(r2, 1e-6));
        float contrib = saturate(nDotV - ssaoBias) * falloff;

        occlusion += contrib;
        if (giEnabled > 0.5)
            bounce += Fetch(sp, size) * contrib;
    }

    float ao = saturate(1.0 - occlusion * ssaoIntensity / float(SSAO_SAMPLES));
    bounce = bounce * (giIntensity / float(SSAO_SAMPLES));
    return float4(bounce, ao);
}

float3 ApplyGrade(float3 c)
{
    c *= exp2(exposure);                       // exposure in stops
    c.r *= 1.0 - temperature * 0.10;           // white balance: warm(-) / cool(+)
    c.b *= 1.0 + temperature * 0.10;
    c.g *= 1.0 + tint * 0.10;                   // tint: magenta(-) / green(+)
    c = (c - 0.5) * contrast + 0.5;            // contrast around mid grey

    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    float sat = saturate(max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b));
    float satScale = saturation + vibrance * (1.0 - sat); // vibrance boosts flat colors more
    c = lerp(luma.xxx, c, satScale);

    if (filmic > 0.5)                          // ACES-approximation filmic curve
        c = (c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14);

    return saturate(c);
}

float4 PSMain(VSOut input) : SV_Target
{
    int2 size;
    srcTex.GetDimensions(size.x, size.y);
    int2 p = int2(input.pos.xy);


    // Panini remaps which part of the rendered frame lands on this pixel. Everything
    // downstream then works from that remapped position so the effects stay aligned.
    int2 ps = p;
    float3 e;
    if (paniniEnabled > 0.5) {
        float2 psf = PaniniSource(p, size);
        ps = int2(round(psf));
        e = FetchBilinear(psf, size);   // bilinear: the remap lands between pixels
    } else {
        e = Fetch(p, size);
    }

    float3 outColor = e;

    if (sharpenEnabled > 0.5) {
        float3 a = Fetch(ps + int2(-1, -1), size);
        float3 b = Fetch(ps + int2( 0, -1), size);
        float3 c = Fetch(ps + int2( 1, -1), size);
        float3 d = Fetch(ps + int2(-1,  0), size);
        float3 f = Fetch(ps + int2( 1,  0), size);
        float3 g = Fetch(ps + int2(-1,  1), size);
        float3 h = Fetch(ps + int2( 0,  1), size);
        float3 i = Fetch(ps + int2( 1,  1), size);

        float3 mnRGB  = min(min(min(d, e), min(f, b)), h);
        float3 mnRGB2 = min(mnRGB, min(min(a, c), min(g, i)));
        mnRGB += mnRGB2;
        float3 mxRGB  = max(max(max(d, e), max(f, b)), h);
        float3 mxRGB2 = max(mxRGB, max(max(a, c), max(g, i)));
        mxRGB += mxRGB2;

        float3 rcpMxRGB = rcp(max(mxRGB, 1e-5));
        float3 ampRGB = sqrt(saturate(min(mnRGB, 2.0 - mxRGB) * rcpMxRGB));
        float peak = -rcp(lerp(8.0, 5.0, saturate(sharpness)));
        float3 wRGB = ampRGB * peak;
        float3 rcpWeightRGB = rcp(1.0 + 4.0 * wRGB);

        float3 window = b + d + f + h;
        outColor = saturate((window * wRGB + e) * rcpWeightRGB);
    }

    // Ambient occlusion darkens creases and contact points; the optional GI term adds one
    // bounce of colour gathered from the surrounding surface. Both run before grading so
    // the grade sees the lit result.
    if (ssaoEnabled > 0.5 && depthAvailable > 0.5) {
        float4 gi = ComputeSSAO(ps, size);
        outColor = outColor * gi.a + gi.rgb;
    }

    if (gradeEnabled > 0.5)
        outColor = ApplyGrade(outColor);

    if (lutEnabled > 0.5 && lutSize > 1.5) {
        // Half-texel-inset coordinate so the LUT endpoints map exactly to 0 and 1.
        float3 uvw = saturate(outColor) * ((lutSize - 1.0) / lutSize) + (0.5 / lutSize);
        float3 graded = lutTex.SampleLevel(lutSampler, uvw, 0.0).rgb;
        outColor = lerp(outColor, graded, lutIntensity);
    }

    // Debug output is drawn as a small inset in the corner rather than over the whole
    // screen, so the game stays visible and playable while a diagnostic is displayed.
    if (debugView > 0.5) {
        float2 insetSize = floor(float2(size) * 0.28);
        float2 origin = float2(size) - insetSize - 24.0;
        float2 rel = float2(p) - origin;
        if (rel.x >= 0.0 && rel.y >= 0.0 && rel.x < insetSize.x && rel.y < insetSize.y) {
            if (rel.x < 2.0 || rel.y < 2.0 ||
                rel.x >= insetSize.x - 2.0 || rel.y >= insetSize.y - 2.0)
                return float4(0.10, 0.85, 0.30, 1.0); // border

            if (depthAvailable < 0.5)
                return float4(1.0, 0.0, 1.0, 1.0);    // magenta: no depth captured

            int2 sp = clamp(int2(rel / insetSize * float2(size)), int2(0, 0), size - 1);
            if (debugView < 1.5) {
                float d = depthTex.Load(int3(sp, 0)).r;
                float v = pow(saturate(d), max(debugGamma, 0.0001));
                return float4(v, v, v, 1.0);
            }
            float ao = ComputeSSAO(sp, size).a;
            return float4(ao, ao, ao, 1.0);
        }
    }

    return float4(outColor, 1.0);
}
)hlsl";

ID3D11VertexShader* s_vs = nullptr;
ID3D11PixelShader* s_ps = nullptr;
ID3D11Buffer* s_cbuffer = nullptr;
ID3D11Texture2D* s_copy = nullptr;
ID3D11ShaderResourceView* s_copySRV = nullptr;
ID3D11RenderTargetView* s_backRTV = nullptr;
UINT s_width = 0, s_height = 0;
DXGI_FORMAT s_format = DXGI_FORMAT_UNKNOWN;
bool s_initTried = false;
bool s_initOk = false;
bool s_formatWarned = false;

// LUT resources are resolution-independent — created/destroyed only by SetLut/ClearLut.
ID3D11Texture3D* s_lutTex = nullptr;
ID3D11ShaderResourceView* s_lutSRV = nullptr;
ID3D11SamplerState* s_lutSampler = nullptr;
int s_lutSize = 0;

// Non-owning; valid only for the frame it was set on.
ID3D11ShaderResourceView* s_depthSRV = nullptr;

template <typename T>
void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Maps typeless families to a concrete view format; returns UNKNOWN when unsupported.
DXGI_FORMAT ViewFormat(DXGI_FORMAT f)
{
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return f;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

bool InitShaders(ID3D11Device* device)
{
    if (s_initTried)
        return s_initOk;
    s_initTried = true;

    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;

    HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "post.hlsl", nullptr, nullptr,
                            "VSMain", "vs_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) {
        log::Write("post: VS compile failed (%08X): %s", hr,
                   errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no log");
        SafeRelease(errors);
        return false;
    }
    hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_vs);
    SafeRelease(blob);
    SafeRelease(errors);
    if (FAILED(hr)) {
        log::Write("post: CreateVertexShader failed (%08X)", hr);
        return false;
    }

    hr = D3DCompile(kShader, sizeof(kShader) - 1, "post.hlsl", nullptr, nullptr,
                    "PSMain", "ps_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) {
        log::Write("post: PS compile failed (%08X): %s", hr,
                   errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no log");
        SafeRelease(errors);
        return false;
    }
    hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_ps);
    SafeRelease(blob);
    SafeRelease(errors);
    if (FAILED(hr)) {
        log::Write("post: CreatePixelShader failed (%08X)", hr);
        return false;
    }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 128; // 32 floats, 16-byte aligned
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbd, nullptr, &s_cbuffer);
    if (FAILED(hr)) {
        log::Write("post: constant buffer creation failed (%08X)", hr);
        return false;
    }

    D3D11_SAMPLER_DESC sad = {};
    sad.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sad.AddressU = sad.AddressV = sad.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sad.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sad, &s_lutSampler);
    if (FAILED(hr)) {
        log::Write("post: LUT sampler creation failed (%08X)", hr);
        return false;
    }

    s_initOk = true;
    log::Write("post: shaders compiled");
    return true;
}

bool EnsureTargets(ID3D11Device* device, ID3D11Texture2D* backbuffer)
{
    D3D11_TEXTURE2D_DESC desc;
    backbuffer->GetDesc(&desc);

    if (s_copy && desc.Width == s_width && desc.Height == s_height && desc.Format == s_format)
        return true;

    OnResize();

    DXGI_FORMAT view = ViewFormat(desc.Format);
    if (view == DXGI_FORMAT_UNKNOWN) {
        if (!s_formatWarned) {
            s_formatWarned = true;
            log::Write("post: unsupported backbuffer format %d — post processing disabled", desc.Format);
        }
        return false;
    }

    D3D11_TEXTURE2D_DESC cd = desc;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cd.MiscFlags = 0;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.CPUAccessFlags = 0;
    HRESULT hr = device->CreateTexture2D(&cd, nullptr, &s_copy);
    if (FAILED(hr)) {
        log::Write("post: copy texture creation failed (%08X)", hr);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format = view;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(s_copy, &srvd, &s_copySRV);
    if (FAILED(hr)) {
        log::Write("post: SRV creation failed (%08X)", hr);
        OnResize();
        return false;
    }

    D3D11_RENDER_TARGET_VIEW_DESC rtvd = {};
    rtvd.Format = view;
    rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = device->CreateRenderTargetView(backbuffer, &rtvd, &s_backRTV);
    if (FAILED(hr)) {
        log::Write("post: RTV creation failed (%08X)", hr);
        OnResize();
        return false;
    }

    s_width = desc.Width;
    s_height = desc.Height;
    s_format = desc.Format;
    return true;
}

// Full save/restore of every pipeline stage the pass touches.
struct StateBackup {
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11InputLayout* inputLayout;
    ID3D11VertexShader* vs;
    ID3D11ClassInstance* vsInstances[256];
    UINT vsInstanceCount;
    ID3D11PixelShader* ps;
    ID3D11ClassInstance* psInstances[256];
    UINT psInstanceCount;
    ID3D11GeometryShader* gs;
    ID3D11HullShader* hs;
    ID3D11DomainShader* ds;
    ID3D11ComputeShader* cs;
    ID3D11Buffer* psCB;
    ID3D11ShaderResourceView* psSRV;
    ID3D11SamplerState* psSampler;
    ID3D11BlendState* blend;
    float blendFactor[4];
    UINT sampleMask;
    ID3D11DepthStencilState* depth;
    UINT stencilRef;
    ID3D11RasterizerState* raster;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView* dsv;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT viewportCount;

    void Capture(ID3D11DeviceContext* ctx)
    {
        vsInstanceCount = 256;
        psInstanceCount = 256;
        ctx->IAGetPrimitiveTopology(&topology);
        ctx->IAGetInputLayout(&inputLayout);
        ctx->VSGetShader(&vs, vsInstances, &vsInstanceCount);
        ctx->PSGetShader(&ps, psInstances, &psInstanceCount);
        ctx->GSGetShader(&gs, nullptr, nullptr);
        ctx->HSGetShader(&hs, nullptr, nullptr);
        ctx->DSGetShader(&ds, nullptr, nullptr);
        ctx->CSGetShader(&cs, nullptr, nullptr);
        ctx->PSGetConstantBuffers(0, 1, &psCB);
        ctx->PSGetShaderResources(0, 1, &psSRV);
        ctx->PSGetSamplers(0, 1, &psSampler);
        ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
        ctx->OMGetDepthStencilState(&depth, &stencilRef);
        ctx->RSGetState(&raster);
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&viewportCount, viewports);
    }

    void Restore(ID3D11DeviceContext* ctx)
    {
        ctx->IASetPrimitiveTopology(topology);
        ctx->IASetInputLayout(inputLayout);
        SafeRelease(inputLayout);
        ctx->VSSetShader(vs, vsInstances, vsInstanceCount);
        SafeRelease(vs);
        for (UINT i = 0; i < vsInstanceCount; ++i)
            SafeRelease(vsInstances[i]);
        ctx->PSSetShader(ps, psInstances, psInstanceCount);
        SafeRelease(ps);
        for (UINT i = 0; i < psInstanceCount; ++i)
            SafeRelease(psInstances[i]);
        ctx->GSSetShader(gs, nullptr, 0);
        SafeRelease(gs);
        ctx->HSSetShader(hs, nullptr, 0);
        SafeRelease(hs);
        ctx->DSSetShader(ds, nullptr, 0);
        SafeRelease(ds);
        ctx->CSSetShader(cs, nullptr, 0);
        SafeRelease(cs);
        ctx->PSSetConstantBuffers(0, 1, &psCB);
        SafeRelease(psCB);
        ctx->PSSetShaderResources(0, 1, &psSRV);
        SafeRelease(psSRV);
        ctx->PSSetSamplers(0, 1, &psSampler);
        SafeRelease(psSampler);
        ctx->OMSetBlendState(blend, blendFactor, sampleMask);
        SafeRelease(blend);
        ctx->OMSetDepthStencilState(depth, stencilRef);
        SafeRelease(depth);
        ctx->RSSetState(raster);
        SafeRelease(raster);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        for (auto& rtv : rtvs)
            SafeRelease(rtv);
        SafeRelease(dsv);
        ctx->RSSetViewports(viewportCount, viewports);
    }
};

} // namespace

void SetDepthSrv(ID3D11ShaderResourceView* srv)
{
    s_depthSRV = srv;
}

float RequiredSourceFovDegrees(float displayedFovDegrees, float strength, float zoom)
{
    // Mirrors PaniniSource() for the screen edge (ndc.x = 1): find the view angle Panini
    // places there, which is the angle the engine must actually render out to.
    constexpr float kPi = 3.14159265f;
    const float tanHalf = tanf(displayedFovDegrees * 0.5f * kPi / 180.0f);
    const float x = tanHalf * zoom;
    const float d = strength;
    const float dd = d + 1.0f;

    const float A = x * x + dd * dd;
    const float B = 2.0f * x * x * d;
    const float C = x * x * d * d - dd * dd;
    float disc = B * B - 4.0f * A * C;
    if (disc < 0.0f)
        disc = 0.0f;

    float c = (-B + sqrtf(disc)) / (2.0f * A);
    if (c < 1e-4f) c = 1e-4f;
    if (c > 1.0f) c = 1.0f;

    const float tanTheta = sqrtf(1.0f - c * c) / c;
    return 2.0f * atanf(tanTheta) * 180.0f / kPi;
}

void Apply(IDXGISwapChain* swap, ID3D11Device* device, ID3D11DeviceContext* ctx)
{
    if (!g_grade.enabled && !g_sharpenEnabled && !(g_lutEnabled && s_lutSize > 1) &&
        !g_ssao.enabled && !g_panini.enabled && g_debugView == 0)
        return;
    if (!InitShaders(device))
        return;

    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer))))
        return;

    if (!EnsureTargets(device, backbuffer)) {
        backbuffer->Release();
        return;
    }

    ctx->CopyResource(s_copy, backbuffer);
    backbuffer->Release();

    bool lutActive = g_lutEnabled && s_lutSRV && s_lutSize > 1;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(s_cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        const float aspect = s_height ? float(s_width) / float(s_height) : 1.777f;
        const float tanHalfFovX = tanf(g_ssao.fovDegrees * 0.5f * 3.14159265f / 180.0f);
        const float tanHalfFovY = tanHalfFovX / aspect;

        float params[32] = {
            g_sharpness,
            g_grade.exposure,
            g_grade.contrast,
            g_grade.saturation,
            g_grade.vibrance,
            g_grade.temperature,
            g_grade.tint,
            g_grade.filmic ? 1.0f : 0.0f,
            g_grade.enabled ? 1.0f : 0.0f,
            g_sharpenEnabled ? 1.0f : 0.0f,
            lutActive ? 1.0f : 0.0f,
            g_lutIntensity,
            float(s_lutSize),
            float(g_debugView),
            g_debugGamma,
            s_depthSRV ? 1.0f : 0.0f,

            g_ssao.enabled ? 1.0f : 0.0f,
            g_ssao.intensity,
            g_ssao.radius,
            g_ssao.bias,

            g_ssao.nearZ,
            g_ssao.farZ,
            tanHalfFovX,
            tanHalfFovY,

            g_ssao.gi ? 1.0f : 0.0f,
            g_ssao.giIntensity,
            g_panini.enabled ? 1.0f : 0.0f,
            g_panini.strength,

            g_panini.zoom,
            0.0f, 0.0f, 0.0f,
        };
        memcpy(mapped.pData, params, sizeof(params));
        ctx->Unmap(s_cbuffer, 0);
    }

    StateBackup backup;
    backup.Capture(ctx);

    D3D11_VIEWPORT vp = {};
    vp.Width = float(s_width);
    vp.Height = float(s_height);
    vp.MaxDepth = 1.0f;

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);
    ctx->OMSetRenderTargets(1, &s_backRTV, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(s_vs, nullptr, 0);
    ctx->PSSetShader(s_ps, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &s_cbuffer);
    ctx->PSSetShaderResources(0, 1, &s_copySRV);
    if (lutActive) {
        ctx->PSSetShaderResources(1, 1, &s_lutSRV);
        ctx->PSSetSamplers(0, 1, &s_lutSampler);
    }
    if (s_depthSRV)
        ctx->PSSetShaderResources(2, 1, &s_depthSRV);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetState(nullptr);
    ctx->Draw(3, 0);

    backup.Restore(ctx);

    // StateBackup restores slot 0 (t0/s0) but not t1/t2 — release those bindings explicitly
    // so the engine's depth buffer is never left bound as an SRV while it writes depth.
    if (lutActive)
        ctx->PSSetShaderResources(1, 1, &nullSRV);
    if (s_depthSRV)
        ctx->PSSetShaderResources(2, 1, &nullSRV);
}

void OnResize()
{
    SafeRelease(s_backRTV);
    SafeRelease(s_copySRV);
    SafeRelease(s_copy);
    s_width = s_height = 0;
    s_format = DXGI_FORMAT_UNKNOWN;
}

bool SetLut(ID3D11Device* device, const float* rgba, int size)
{
    if (!device || !rgba || size < 2)
        return false;

    ID3D11Texture3D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;

    D3D11_TEXTURE3D_DESC td = {};
    td.Width = UINT(size);
    td.Height = UINT(size);
    td.Depth = UINT(size);
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = rgba;
    init.SysMemPitch = UINT(size) * 4 * sizeof(float);          // one row
    init.SysMemSlicePitch = UINT(size) * UINT(size) * 4 * sizeof(float); // one depth slice

    if (FAILED(device->CreateTexture3D(&td, &init, &tex)) || !tex) {
        log::Write("post: LUT Texture3D creation failed (size %d)", size);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = td.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    sd.Texture3D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(tex, &sd, &srv))) {
        tex->Release();
        log::Write("post: LUT SRV creation failed");
        return false;
    }

    SafeRelease(s_lutSRV);
    SafeRelease(s_lutTex);
    s_lutTex = tex;
    s_lutSRV = srv;
    s_lutSize = size;
    log::Write("post: LUT loaded (%d^3)", size);
    return true;
}

void ClearLut()
{
    SafeRelease(s_lutSRV);
    SafeRelease(s_lutTex);
    s_lutSize = 0;
}

int LutSize()
{
    return s_lutSize;
}

void Shutdown()
{
    OnResize();
    ClearLut();
    SafeRelease(s_lutSampler);
    SafeRelease(s_cbuffer);
    SafeRelease(s_ps);
    SafeRelease(s_vs);
}

}
