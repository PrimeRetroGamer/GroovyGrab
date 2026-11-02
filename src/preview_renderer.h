#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <cstdint>

// The D3D11 preview: draws the latest capture sample as a 4:3 picture, with the
// same crop and letterbox as the CRT output. The preview never feeds the stream.
struct PreviewRenderer {
    explicit PreviewRenderer(HWND window);

    void upload_rgb(const uint8_t* rgb, LONG pitch, UINT width, UINT height);
    void upload_yuy2(const uint8_t* yuy2, LONG pitch, UINT width, UINT height);
    void clear_image() { has_image = false; }

    void draw(float dest_x, float dest_y, float dest_w, float dest_h, bool letterbox, int crop_h,
              int crop_v);
    bool present_frame(bool allow_tearing, bool nonblocking);

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    bool tearing_supported = false;

private:
    void ensure_textures(UINT width, UINT height);
    void resize_if_needed();

    HWND hwnd{};
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapchain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> bgra_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> bgra_view;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> yuy2_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> yuy2_view;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> bgra_shader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> yuy2_shader;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> preview_cb;
    UINT swapchain_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    UINT backbuffer_width = 0;
    UINT backbuffer_height = 0;
    UINT video_width = 0;
    UINT video_height = 0;
    bool has_image = false;
    bool image_is_rgb = false;
};
