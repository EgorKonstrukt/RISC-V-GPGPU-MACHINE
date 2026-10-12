// Runs the display stream's shaders (unity/ShaderEmu/Shaders/ShareUnits.shader and
// ShareUnitsDecode.shader, docs/share.md "Units") outside Unity, on pictures of a display:
//
//   share_test FIRST.png NEXT.png ... [--room 1131] [--most 400] [--out FOLDER] [--no-pictures] [--lost-at N]
//
// Sends FIRST until nothing is left, then each NEXT for one packet (the last until nothing is
// left), and writes packet_N.bin, state_N.bin and picture_N.bin: tools/share_units.py does the
// same in numpy and compares (--against FOLDER). --lost-at N: when packet N is made, packets
// N - 3 and N - 2 are said to be lost. --time: what each pass costs, after the last packet.
#include "common.h"
#include "gpu.h"
#include "image.h"
#include "material.h"
#include "shaderlab.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <objbase.h>

namespace {

struct Target {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    UINT w = 0, h = 0;
};

Gpu gpu;
ComPtr<ID3D11VertexShader> triangle;

bool makeTarget(Target& t, UINT w, UINT h) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(gpu.device->CreateTexture2D(&td, nullptr, &t.tex))) return false;
    if (FAILED(gpu.device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv))) return false;
    if (FAILED(gpu.device->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv))) return false;
    const float zero[4] = {0, 0, 0, 0};
    gpu.ctx->ClearRenderTargetView(t.rtv.Get(), zero);
    t.w = w;
    t.h = h;
    return true;
}

// As VRCGraphics.Blit with a material's pass: the whole target, the pass's fragment shader.
void blit(Material& mat, GpuPass& pass, Target& to) {
    ID3D11DeviceContext* ctx = gpu.ctx.Get();
    ctx->OMSetRenderTargets(1, to.rtv.GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp{0, 0, (float)to.w, (float)to.h, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(gpu.rasterNoCull.Get());
    ctx->OMSetBlendState(gpu.blendOpaque.Get(), nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(gpu.depthOff.Get(), 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mat.bind(ctx, pass, gpu);
    ctx->VSSetShader(triangle.Get(), nullptr, 0);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    for (auto& t : pass.psLayout.textures) ctx->PSSetShaderResources(t.slot, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

bool readTarget(Target& t, std::vector<uint8_t>& out) {
    D3D11_TEXTURE2D_DESC td{};
    t.tex->GetDesc(&td);
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(gpu.device->CreateTexture2D(&td, nullptr, &staging))) return false;
    gpu.ctx->CopyResource(staging.Get(), t.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(gpu.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    out.resize((size_t)t.w * t.h * 4);
    for (UINT y = 0; y < t.h; y++) memcpy(&out[(size_t)y * t.w * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)t.w * 4);
    gpu.ctx->Unmap(staging.Get(), 0);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> frames;
    std::string out, shaders = "unity/ShaderEmu/Shaders";
    int room = 1131, most = 400, lostAt = 0;
    bool pictures = true, timePasses = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--room" && i + 1 < argc) room = atoi(argv[++i]);
        else if (a == "--most" && i + 1 < argc) most = atoi(argv[++i]);
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--shaders" && i + 1 < argc) shaders = argv[++i];
        else if (a == "--no-pictures") pictures = false;
        else if (a == "--time") timePasses = true;
        else if (a == "--lost-at" && i + 1 < argc) lostAt = atoi(argv[++i]);
        else frames.push_back(a);
    }
    if (frames.empty()) {
        fprintf(stderr, "usage: share_test FIRST.png NEXT.png ... [--room N] [--most N] [--out FOLDER] [--no-pictures] [--shaders FOLDER]\n");
        return 2;
    }
    std::string err;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // (the pictures are read through WIC)
    if (!gpu.init(GpuOptions{}, err)) {
        fprintf(stderr, "share_test: %s\n", err.c_str());
        return 1;
    }
    ShaderBuildOptions bo;
    bo.settings.cacheDir = "build/shadercache";
    bo.settings.includeDirs = {SHADEREMU_UNITY_INCLUDE_DIR};
    bo.settings.defines = {{"UNITY_COLORSPACE_GAMMA", "1"}, {"SHADER_API_D3D11", "1"}};
    SLShader encode, decode;
    std::vector<GpuPass> ep, dp;
    auto t0 = std::chrono::steady_clock::now();
    if (!loadShaderLab(shaders + "/ShareUnits.shader", encode, err) ||
        !buildPasses(gpu, encode, {"Blocks", "Exact", "Need", "Marks", "Stats", "Plan", "Units", "Prefix", "Packet", "State"}, bo, ep, err) ||
        !loadShaderLab(shaders + "/ShareUnitsDecode.shader", decode, err) || !buildPasses(gpu, decode, {"Index", "Draw"}, bo, dp, err)) {
        fprintf(stderr, "share_test: %s\n", err.c_str());
        return 1;
    }
    // the whole target; uv as a Unity blit has it for these shaders: 1 at the top row
    const char* vs = "struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
                     "O main(uint id : SV_VertexID) { O o; float2 p = float2((id << 1) & 2, id & 2);\n"
                     "o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = float2(p.x, 1.0 - p.y); return o; }\n";
    StageResult sr = compileStage(vs, "triangle", ".", "main", "vs_5_0", {}, bo.settings);
    if (!sr.ok || FAILED(gpu.device->CreateVertexShader(sr.bytecode->GetBufferPointer(), sr.bytecode->GetBufferSize(), nullptr, &triangle))) {
        fprintf(stderr, "share_test: %s\n", sr.log.c_str());
        return 1;
    }
    printf("shaders ready in %.1f s (%s)\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), shaderCompilerName());

    enum { Blocks, Exact, Need, Marks, Stats, Plan, Units, Prefix, Packet, State };
    Target blocks, exact, need, state[3], stats, plan, units, prefix, packet, index, picture;
    bool ok = makeTarget(blocks, 640, 450) && makeTarget(exact, 160, 210) && makeTarget(need, 160, 120) && makeTarget(stats, 1, 1) &&
              makeTarget(plan, 160, 120) && makeTarget(units, 80, 113) && makeTarget(prefix, 256, 1) && makeTarget(packet, 292, 1) &&
              makeTarget(index, 80, 113) && makeTarget(picture, 1280, 960);
    for (auto& s : state) ok = ok && makeTarget(s, 160, 120);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 288;
    td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> received;
    ComPtr<ID3D11ShaderResourceView> receivedSrv;
    ok = ok && SUCCEEDED(gpu.device->CreateTexture2D(&td, nullptr, &received)) && SUCCEEDED(gpu.device->CreateShaderResourceView(received.Get(), nullptr, &receivedSrv));
    if (!ok) {
        fprintf(stderr, "share_test: no targets\n");
        return 1;
    }
    if (!out.empty()) std::filesystem::create_directories(out);

    Material mat, show;
    int committed = 0, number = 0, total = 0;
    ComPtr<ID3D11ShaderResourceView> before;
    double seconds = 0;
    for (size_t f = 0; f < frames.size(); f++) {
        ImageRGBA8 img, padded;
        if (!loadImageRGBA8(frames[f], img, err) || img.width > 1280 || img.height > 960) {
            fprintf(stderr, "share_test: %s: %s\n", frames[f].c_str(), err.empty() ? "larger than 1280 x 960" : err.c_str());
            return 1;
        }
        padded.width = 1280;
        padded.height = 960;
        padded.pixels.assign(1280 * 960 * 4, 0);
        for (UINT y = 0; y < img.height; y++) memcpy(&padded.pixels[(size_t)y * 1280 * 4], &img.pixels[(size_t)y * img.width * 4], (size_t)img.width * 4);
        ComPtr<ID3D11ShaderResourceView> now = createTextureRGBA8(gpu.device.Get(), padded, false, err);
        if (!now) {
            fprintf(stderr, "share_test: %s\n", err.c_str());
            return 1;
        }
        bool hold = f == 0 || f + 1 == frames.size();
        int sent = 0;
        while (number < most) {
            auto start = std::chrono::steady_clock::now();
            number++;
            Target &was = state[committed], &marked = state[(committed + 1) % 3], &after = state[(committed + 2) % 3];
            mat.setTexture("_Now", now.Get(), 1280, 960);
            mat.setTexture("_Before", before ? before.Get() : now.Get(), 1280, 960);
            mat.setVector("_Size", img.width, img.height, 0, 0);
            mat.setVector("_Lost", (number - 3) & 255, number == lostAt ? 2 : 0, 0, 0);
            mat.setFloat("_Reset", before ? 0 : 1);
            mat.setFloat("_Room", room);
            mat.setFloat("_Seq", number & 255);
            mat.setFloat("_Turn", (number % 4800) * 7919 % 4800);
            blit(mat, ep[Blocks], blocks);
            mat.setTexture("_Blocks", blocks.srv.Get(), blocks.w, blocks.h);
            blit(mat, ep[Exact], exact);
            mat.setTexture("_Exact", exact.srv.Get(), exact.w, exact.h);
            blit(mat, ep[Need], need);
            mat.setTexture("_Need", need.srv.Get(), need.w, need.h);
            mat.setTexture("_State", was.srv.Get(), 160, 120);
            blit(mat, ep[Marks], marked);
            mat.setTexture("_State", marked.srv.Get(), 160, 120);
            blit(mat, ep[Stats], stats);
            mat.setTexture("_Stats", stats.srv.Get(), 1, 1);
            blit(mat, ep[Plan], plan);
            mat.setTexture("_Plan", plan.srv.Get(), plan.w, plan.h);
            blit(mat, ep[Units], units);
            mat.setTexture("_Units", units.srv.Get(), units.w, units.h);
            blit(mat, ep[Prefix], prefix);
            mat.setTexture("_Prefix", prefix.srv.Get(), prefix.w, prefix.h);
            blit(mat, ep[Packet], packet);
            blit(mat, ep[State], after);
            committed = (committed + 2) % 3;
            before = now;

            std::vector<uint8_t> bytes, cells, shown;
            if (!readTarget(packet, bytes) || !readTarget(state[committed], cells)) return 1;
            int length = bytes[288 * 4] | bytes[288 * 4 + 1] << 8;
            // the receiver: the packet's bytes as a texture, each unit's place in it, the picture
            std::vector<uint8_t> got(288 * 4, 0);
            memcpy(got.data(), bytes.data(), length);
            gpu.ctx->UpdateSubresource(received.Get(), 0, nullptr, got.data(), 288 * 4, 0);
            show.setTexture("_Packet", receivedSrv.Get(), 288, 1);
            show.setFloat("_Length", length);
            show.setVector("_Stream", img.width, img.height, 0, 0);
            show.setVector("_TargetSize", 1280, 960, 0, 0);
            blit(show, dp[0], index);
            show.setTexture("_Index", index.srv.Get(), index.w, index.h);
            blit(show, dp[1], picture);
            if (pictures && !readTarget(picture, shown)) return 1;
            seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (!out.empty()) {
                std::string n = std::to_string(number);
                writeFileBinary(out + "/packet_" + n + ".bin", bytes.data(), length);
                writeFileBinary(out + "/state_" + n + ".bin", cells.data(), cells.size());
                if (pictures) writeFileBinary(out + "/picture_" + n + ".bin", shown.data(), shown.size());
            }
            total += length;
            sent++;
            if (!hold || length == 0) break;
        }
        printf("%-28s %3d packets, %6d bytes so far\n", std::filesystem::path(frames[f]).filename().string().c_str(), sent, total);
    }
    if (timePasses) {
        // each pass 50 times over with the last packet's textures, then waited for
        const char* names[] = {"Blocks", "Exact", "Need", "Marks", "Stats", "Plan", "Units", "Prefix", "Packet", "State", "Index", "Draw"};
        Target* to[] = {&blocks, &exact, &need, &state[(committed + 1) % 3], &stats, &plan, &units, &prefix, &packet, &state[(committed + 2) % 3], &index, &picture};
        std::vector<uint8_t> wait;
        double all = 0;
        for (int k = 0; k < 12; k++) {
            if (k == Marks || k == State) mat.setTexture("_State", state[committed].srv.Get(), 160, 120);
            readTarget(stats, wait);
            auto start = std::chrono::steady_clock::now();
            for (int n = 0; n < 50; n++) blit(k < 10 ? mat : show, k < 10 ? ep[k] : dp[k - 10], *to[k]);
            readTarget(stats, wait);
            double ms = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() * 20;
            printf("  %-7s %6.3f ms\n", names[k], ms);
            all += ms;
        }
        printf("  all     %6.3f ms\n", all);
    }
    std::string lost = gpu.deviceRemovedReason();
    if (!lost.empty()) {
        fprintf(stderr, "share_test: %s\n", lost.c_str());
        return 1;
    }
    printf("%d packets in %.2f s: %.1f ms each with their readbacks\n", number, seconds, 1000 * seconds / (number ? number : 1));
    return 0;
}
