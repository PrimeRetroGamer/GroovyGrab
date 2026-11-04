#include "preview_renderer.h"

#include "directshow_capture.h"
#include "video_view.h"

#include <d3dcompiler.h>
#include <dxgi1_5.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

using directshow_capture::check;
using Microsoft::WRL::ComPtr;

PreviewRenderer::PreviewRenderer(HWND window) : hwnd(window) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                 D3D11_CREATE_DEVICE_SINGLETHREADED;
    D3D_FEATURE_LEVEL level{};
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                            flags, nullptr, 0, D3D11_SDK_VERSION,
                            &device, &level, &context),
          "D3D11CreateDevice");

    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    check(device.As(&dxgi_device), "query IDXGIDevice");
    check(dxgi_device->GetAdapter(&adapter), "GetAdapter");
    check(adapter->GetParent(IID_PPV_ARGS(&factory)), "GetParent(factory)");

    ComPtr<IDXGIFactory5> factory5;
    BOOL allow_tearing = FALSE;
    if (SUCCEEDED(factory.As(&factory5)) &&
        SUCCEEDED(factory5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
            sizeof(allow_tearing))) && allow_tearing) {
        tearing_supported = true;
        swapchain_flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = swapchain_flags;
    ComPtr<IDXGISwapChain1> swapchain1;
    check(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &desc,
                                           nullptr, nullptr, &swapchain1),
          "CreateSwapChainForHwnd");
    check(swapchain1.As(&swapchain), "query IDXGISwapChain2");
    check(swapchain->SetMaximumFrameLatency(1),
          "SetMaximumFrameLatency(1)");

    static constexpr char shader_source[] = R"(
        struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
        VOut vs_main(uint id : SV_VertexID) {
            float2 p[4] = { float2(-1, 1), float2(1, 1),
                            float2(-1,-1), float2(1,-1) };
            float2 u[4] = { float2(0,0), float2(1,0),
                            float2(0,1), float2(1,1) };
            VOut o; o.pos = float4(p[id], 0, 1); o.uv = u[id]; return o;
        }
        Texture2D<float4> image : register(t0);
        SamplerState image_sampler : register(s0);
        cbuffer Preview : register(b0) {
            float4 src_uv;
            float4 dst_uv;
            float4 size;
        };
        float4 ps_bgra(VOut i) : SV_Target {
            float2 uv = i.uv;
            if (uv.x < dst_uv.x || uv.y < dst_uv.y ||
                uv.x >= dst_uv.z || uv.y >= dst_uv.w) {
                return float4(0, 0, 0, 1);
            }
            float2 content = max(dst_uv.zw - dst_uv.xy, float2(1e-6, 1e-6));
            float2 t = saturate((uv - dst_uv.xy) / content);
            float2 s = lerp(src_uv.xy, src_uv.zw, t);
            return image.SampleLevel(image_sampler, s, 0);
        }
        float3 yuy2_rgb(uint x, uint y) {
            uint packed_w = 0, packed_h = 0;
            image.GetDimensions(packed_w, packed_h);
            uint pix_w = packed_w * 2;
            x = min(x, pix_w - 1);
            y = min(y, packed_h - 1);
            float4 pair = image.Load(int3(int(x >> 1), int(y), 0));
            float luma = ((x & 1) != 0) ? pair.b : pair.r;
            float yy = max(0.0, (luma * 255.0 - 16.0) * (1.0 / 219.0));
            float u = (pair.g * 255.0 - 128.0) * (1.0 / 224.0);
            float v = (pair.a * 255.0 - 128.0) * (1.0 / 224.0);
            float3 rgb;
            rgb.r = yy + 1.4020 * v;
            rgb.g = yy - 0.344136 * u - 0.714136 * v;
            rgb.b = yy + 1.7720 * u;
            return saturate(rgb);
        }
        float4 ps_yuy2(VOut i) : SV_Target {
            float2 uv = i.uv;
            if (uv.x < dst_uv.x || uv.y < dst_uv.y ||
                uv.x >= dst_uv.z || uv.y >= dst_uv.w) {
                return float4(0, 0, 0, 1);
            }
            float vp_w = max(size.z, 1.0);
            float vp_h = max(size.w, 1.0);
            float src_w = max(size.x, 1.0);
            float src_h = max(size.y, 1.0);
            float2 content = max(dst_uv.zw - dst_uv.xy, float2(1e-6, 1e-6));
            float2 pixel = float2(1.0 / vp_w, 1.0 / vp_h) / content;
            float2 c0 = saturate((uv - dst_uv.xy) / content - 0.5 * pixel);
            float2 c1 = saturate((uv - dst_uv.xy) / content + 0.5 * pixel);
            float2 s0 = lerp(src_uv.xy, src_uv.zw, c0);
            float2 s1 = lerp(src_uv.xy, src_uv.zw, c1);
            float sx0 = s0.x * src_w;
            float sx1 = max(s1.x * src_w, sx0 + 1e-4);
            float sy0 = s0.y * src_h;
            float sy1 = max(s1.y * src_h, sy0 + 1e-4);
            int ix0 = max(int(floor(sx0)), 0);
            int iy0 = max(int(floor(sy0)), 0);
            int ix1 = min(int(ceil(sx1)) - 1, int(src_w) - 1);
            int iy1 = min(int(ceil(sy1)) - 1, int(src_h) - 1);
            if (ix1 > ix0 + 11) ix1 = ix0 + 11;
            if (iy1 > iy0 + 11) iy1 = iy0 + 11;
            float3 acc = float3(0, 0, 0);
            float wsum = 0;
            [loop] for (int y = iy0; y <= iy1; ++y) {
                float yw = min(sy1, float(y + 1)) - max(sy0, float(y));
                if (yw <= 0) continue;
                [loop] for (int x = ix0; x <= ix1; ++x) {
                    float xw = min(sx1, float(x + 1)) - max(sx0, float(x));
                    if (xw <= 0) continue;
                    float w = xw * yw;
                    acc += yuy2_rgb(uint(x), uint(y)) * w;
                    wsum += w;
                }
            }
            if (wsum <= 0) return float4(0, 0, 0, 1);
            return float4(acc / wsum, 1);
        }
    )";
    ComPtr<ID3DBlob> vs_blob, bgra_blob, yuy2_blob, errors;
    check(D3DCompile(shader_source, sizeof(shader_source), nullptr, nullptr,
                     nullptr, "vs_main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
                     0, &vs_blob, &errors),
          "compile vertex shader");
    errors.Reset();
    check(D3DCompile(shader_source, sizeof(shader_source), nullptr, nullptr,
                     nullptr, "ps_bgra", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
                     0, &bgra_blob, &errors),
          "compile BGRA pixel shader");
    errors.Reset();
    check(D3DCompile(shader_source, sizeof(shader_source), nullptr, nullptr,
                     nullptr, "ps_yuy2", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
                     0, &yuy2_blob, &errors),
          "compile YUY2 pixel shader");
    check(device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                     vs_blob->GetBufferSize(), nullptr,
                                     &vertex_shader),
          "CreateVertexShader");
    check(device->CreatePixelShader(bgra_blob->GetBufferPointer(),
                                    bgra_blob->GetBufferSize(), nullptr,
                                    &bgra_shader),
          "CreatePixelShader(BGRA)");
    check(device->CreatePixelShader(yuy2_blob->GetBufferPointer(),
                                    yuy2_blob->GetBufferSize(), nullptr,
                                    &yuy2_shader),
          "CreatePixelShader(YUY2)");

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    check(device->CreateSamplerState(&sampler_desc, &sampler),
          "CreateSamplerState");
    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = 64;
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check(device->CreateBuffer(&cb_desc, nullptr, &preview_cb),
          "CreateBuffer(preview constants)");
    resize_if_needed();
}

void PreviewRenderer::ensure_textures(UINT width, UINT height) {
    if (width == video_width && height == video_height && yuy2_texture) return;
    video_width = width;
    video_height = height;
    bgra_texture.Reset();
    bgra_view.Reset();
    yuy2_texture.Reset();
    yuy2_view.Reset();
    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DYNAMIC;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check(device->CreateTexture2D(&texture_desc, nullptr, &bgra_texture), "CreateTexture2D(BGRA)");
    check(device->CreateShaderResourceView(bgra_texture.Get(), nullptr, &bgra_view),
          "CreateShaderResourceView(BGRA)");
    texture_desc.Width = width / 2;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    check(device->CreateTexture2D(&texture_desc, nullptr, &yuy2_texture),
          "CreateTexture2D(packed YUY2)");
    check(device->CreateShaderResourceView(yuy2_texture.Get(), nullptr, &yuy2_view),
          "CreateShaderResourceView(packed YUY2)");
}

void PreviewRenderer::resize_if_needed() {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    const UINT width = std::max<LONG>(1, rect.right - rect.left);
    const UINT height = std::max<LONG>(1, rect.bottom - rect.top);
    if (width == backbuffer_width && height == backbuffer_height && target) return;
    const HANDLE waitable = swapchain->GetFrameLatencyWaitableObject();
    if (waitable) {
        WaitForSingleObject(waitable, 8);
        CloseHandle(waitable);
    }
    context->OMSetRenderTargets(0, nullptr, nullptr);
    target.Reset();
    backbuffer_width = 0;
    backbuffer_height = 0;
    if (FAILED(swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swapchain_flags))) {
        return;
    }
    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) return;
    if (FAILED(device->CreateRenderTargetView(backbuffer.Get(), nullptr, &target))) return;
    backbuffer_width = width;
    backbuffer_height = height;
}

void PreviewRenderer::upload_rgb(const uint8_t* rgb, LONG pitch, UINT width, UINT height) {
    ensure_textures(width, height);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(bgra_texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
          "Map(RGB preview texture)");
    for (UINT y = 0; y < height; ++y) {
        const uint8_t* in = rgb + static_cast<ptrdiff_t>(y) * pitch;
        uint8_t* out = static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        for (UINT x = 0; x < width; ++x) {
            out[x * 4 + 0] = in[x * 3 + 2];
            out[x * 4 + 1] = in[x * 3 + 1];
            out[x * 4 + 2] = in[x * 3 + 0];
            out[x * 4 + 3] = 255;
        }
    }
    context->Unmap(bgra_texture.Get(), 0);
    has_image = true;
    image_is_rgb = true;
}

void PreviewRenderer::upload_bgra(const uint8_t* bgra, LONG pitch, UINT width, UINT height) {
    ensure_textures(width, height);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(bgra_texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
          "Map(BGRA preview texture)");
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                    bgra + static_cast<ptrdiff_t>(y) * pitch, static_cast<size_t>(width) * 4u);
    }
    context->Unmap(bgra_texture.Get(), 0);
    has_image = true;
    image_is_rgb = true;
}

void PreviewRenderer::upload_yuy2(const uint8_t* yuy2, LONG pitch, UINT width, UINT height) {
    ensure_textures(width, height);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(yuy2_texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
          "Map(video upload texture)");
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                    yuy2 + static_cast<ptrdiff_t>(y) * pitch, width * 2u);
    }
    context->Unmap(yuy2_texture.Get(), 0);
    has_image = true;
    image_is_rgb = false;
}

void PreviewRenderer::draw(float dest_x, float dest_y, float dest_w, float dest_h, bool letterbox,
                           int crop_h, int crop_v) {
    resize_if_needed();
    if (!target) return;

    static constexpr float clear[4] = {18.0f / 255.0f, 10.0f / 255.0f, 28.0f / 255.0f, 1.0f};
    context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
    context->ClearRenderTargetView(target.Get(), clear);
    if (!has_image || dest_w < 1.0f || dest_h < 1.0f) return;

    constexpr float crt_aspect = 4.0f / 3.0f;
    D3D11_VIEWPORT viewport{};
    if (dest_w / dest_h > crt_aspect) {
        viewport.Height = dest_h;
        viewport.Width = dest_h * crt_aspect;
        viewport.TopLeftX = dest_x + (dest_w - viewport.Width) * 0.5f;
        viewport.TopLeftY = dest_y;
    } else {
        viewport.Width = dest_w;
        viewport.Height = dest_w / crt_aspect;
        viewport.TopLeftX = dest_x;
        viewport.TopLeftY = dest_y + (dest_h - viewport.Height) * 0.5f;
    }
    viewport.MaxDepth = 1.0f;

    constexpr unsigned kRasterW = 720, kRasterH = 480;
    const auto map = video_view::make(video_width, video_height, kRasterW, kRasterH, letterbox,
                                      crop_h, crop_v);
    struct PreviewConstants {
        float src_u0, src_v0, src_u1, src_v1;
        float dst_u0, dst_v0, dst_u1, dst_v1;
        float src_w, src_h, vp_w, vp_h;
        float pad[4];
    } constants{};
    const float src_w = static_cast<float>(video_width);
    const float src_h = static_cast<float>(video_height);
    constants.src_u0 = static_cast<float>(map.src_x0) / src_w;
    constants.src_v0 = static_cast<float>(map.src_y0) / src_h;
    constants.src_u1 = static_cast<float>(map.src_x1) / src_w;
    constants.src_v1 = static_cast<float>(map.src_y1) / src_h;
    constants.dst_u0 = static_cast<float>(map.dst_x0) / kRasterW;
    constants.dst_v0 = static_cast<float>(map.dst_y0) / kRasterH;
    constants.dst_u1 = static_cast<float>(map.dst_x1) / kRasterW;
    constants.dst_v1 = static_cast<float>(map.dst_y1) / kRasterH;
    constants.src_w = src_w;
    constants.src_h = src_h;
    constants.vp_w = std::max(1.0f, viewport.Width);
    constants.vp_h = std::max(1.0f, viewport.Height);
    D3D11_MAPPED_SUBRESOURCE cb_mapped{};
    check(context->Map(preview_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cb_mapped),
          "Map(preview constants)");
    std::memcpy(cb_mapped.pData, &constants, sizeof(constants));
    context->Unmap(preview_cb.Get(), 0);

    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(vertex_shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* view = image_is_rgb ? bgra_view.Get() : yuy2_view.Get();
    ID3D11Buffer* cb = preview_cb.Get();
    context->PSSetShader(image_is_rgb ? bgra_shader.Get() : yuy2_shader.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &view);
    context->PSSetSamplers(0, 1, sampler.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, &cb);
    context->Draw(4, 0);
}

bool PreviewRenderer::present_frame(bool allow_tearing, bool nonblocking) {
    UINT present_flags = nonblocking ? DXGI_PRESENT_DO_NOT_WAIT : 0;
    if (allow_tearing && tearing_supported) present_flags |= DXGI_PRESENT_ALLOW_TEARING;
    const HRESULT hr = swapchain->Present(0, present_flags);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
    check(hr, "Present");
    return true;
}
