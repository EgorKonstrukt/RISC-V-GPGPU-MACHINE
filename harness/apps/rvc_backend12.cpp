// D3D12 backend: the same two draws as D3D11, with the shader compiled by DXC to DXIL.

#include "image.h"
#include "rvc_backend.h"
#include "rvc_d3d12.h"
#include "rvc_memview12.h"

#include <deque>
#include <map>

namespace fs = std::filesystem;

namespace {

class Backend12 : public RvcBackend {
public:
    bool init(const BackendOptions& opt, const SLShader& shader, Material&, std::string& err) override {
        if (opt.profile || opt.present) {
            err = "--profile and --present are D3D11 only (use rvc_trace12 for D3D12 profilers)";
            return false;
        }
        tickRows_ = opt.tickRows;
        if (opt.gpu.debug) {
            ComPtr<ID3D12Debug> dbg;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
        }
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        ComPtr<IDXGIAdapter1> adapter;
        if (opt.gpu.warp) check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
        else if (opt.gpu.adapterIndex >= 0 && FAILED(factory->EnumAdapters1((UINT)opt.gpu.adapterIndex, &adapter))) {
            err = "adapter index " + std::to_string(opt.gpu.adapterIndex) + " not found";
            return false;
        }
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dx_.dev)), "D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(dx_.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&dx_.queue)), "queue");
        check(dx_.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx_.fence)), "fence");
        dx_.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fprintf(stderr, "[harness] D3D12 + DXC (shader model %s)%s\n", opt.dxcSm.c_str(), opt.gpu.warp ? " on WARP" : "");

        Build12 b;
        b.dxc = true;
        b.dxcOpt = opt.dxcOpt;
        b.dxcSm = opt.dxcSm;
        b.dxcDir = opt.dxcDir;
        buildPasses12(dx_, shader, opt.compile, b, passes_);
        if (opt.gpuShader) {
            // The control pass reports the workers (MC_STATS) while the tick stays single-core.
            CompileSettings gpuCs = opt.compile;
            if (opt.cores > 1 && opt.computeShader.empty())
                gpuCs.defines.push_back(Defines::value_type("CORES", std::to_string(opt.cores)));
            buildPasses12(dx_, *opt.gpuShader, gpuCs, b, passes_ + 2, "GPUControl", "", 1);
            passCount_ = 3;
            buildGpuDraw(*opt.gpuShader, opt.compile, b);
            textures_["_GpuTarget"] = gpuColor_;
            if (opt.soundShader) buildSoundMix(*opt.soundShader, opt.compile, b);
        }
        if (!opt.computeShader.empty() && !buildComputeCpu(opt.computeShader, opt, err)) return false;

        const uint8_t black[4] = {0, 0, 0, 0};
        blackTex_ = uploadTexture(dx_, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, black, D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        for (UINT s = 0; s < kSlots; ++s) {
            check(dx_.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocs_[s])), "allocator");
            rowBuf_[s] = readbackBuffer(rowFootprint_, kControlTexels, 3, kStateFormat);   // row 0, the control texels, the network's 64
            if (soundPso_) soundBuf_[s] = readbackBuffer(soundFootprint_, kSoundSide, kSoundSide, kSoundFormat);
            if (opt.stateLog) zoneBuf_[s] = readbackBuffer(zoneFootprint_, 64, 64, kStateFormat);
            auto statBd = bufferDesc((UINT64)kCoresMost * kCpuStateBytes);
            auto statHeap = heapProps(D3D12_HEAP_TYPE_READBACK);
            check(dx_.dev->CreateCommittedResource(&statHeap, D3D12_HEAP_FLAG_NONE, &statBd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&statBuf_[s])), "worker stats readback");
        }
        check(dx_.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs_[0].Get(), nullptr, IID_PPV_ARGS(&cl_)), "command list");
        cl_->Close();
        auto cbd = bufferDesc((UINT64)kSlots * kCbPerFrame * kCbBytes);
        auto hu = heapProps(D3D12_HEAP_TYPE_UPLOAD);
        check(dx_.dev->CreateCommittedResource(&hu, D3D12_HEAP_FLAG_NONE, &cbd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&cbuf_)), "constant buffer");
        check(cbuf_->Map(0, nullptr, (void**)&cbMapped_), "Map constants");

        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 4;
        auto tsd = bufferDesc(4 * sizeof(UINT64));
        auto hr = heapProps(D3D12_HEAP_TYPE_READBACK);
        if (FAILED(dx_.dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&tsHeap_))) ||
            FAILED(dx_.dev->CreateCommittedResource(&hr, D3D12_HEAP_FLAG_NONE, &tsd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&tsBuf_))))
            tsHeap_.Reset();
        return true;
    }

    bool loadPayload(Material& mat, const std::string& dir, const std::string& prefix, const std::string& propBase,
                     std::string& err) override {
        if (prefix == "none") return true;
        const char* lanes[4] = {"r", "g", "b", "a"};
        const char* props[4] = {"_R", "_G", "_B", "_A"};
        for (int i = 0; i < 4; ++i) {
            ImageRGBA8 img;
            std::string path = (fs::u8path(dir) / (prefix + "." + lanes[i] + ".png")).u8string();
            if (!loadPayloadLane(dir, prefix, i, img, err)) return false;
            std::vector<uint8_t> flipped(img.pixels.size());  // Unity layout: row 0 = bottom of the image
            size_t pitch = (size_t)img.width * 4;
            for (UINT y = 0; y < img.height; ++y)
                memcpy(flipped.data() + y * pitch, img.pixels.data() + (img.height - 1 - y) * pitch, pitch);
            std::string name = propBase + props[i];
            textures_[name] = uploadTexture(dx_, img.width, img.height, DXGI_FORMAT_R8G8B8A8_UNORM, 4, flipped.data(),
                                            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            mat.setVector(name + "_TexelSize", 1.0 / img.width, 1.0 / img.height, img.width, img.height);
            if (i == 0)
                fprintf(stderr, "[harness] %s_{R,G,B,A} <- %s (%ux%u)\n", propBase.c_str(), prefix.c_str(), img.width, img.height);
        }
        tablesBuilt_ = false;
        return true;
    }

    bool deliverHostData(Material&, const uint8_t* rgba, std::string&) override {
        dx_.flush();
        textures_["_HostData"] = uploadTexture(dx_, kFetchSide, kFetchSide, DXGI_FORMAT_R8G8B8A8_UNORM, 4, rgba,
                                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        tablesBuilt_ = false;
        deliver_ = true;
        return true;
    }

    bool setState(const void* texels, std::string&) override {
        dx_.flush();
        std::vector<uint8_t> zero;
        if (!texels) {
            zero.assign((size_t)kWidth * kHeight * 16, 0);
            texels = zero.data();
        }
        for (int i = 0; i < 2; ++i)
            state_[i] = uploadTexture(dx_, kWidth, kHeight, kStateFormat, 16, (const uint8_t*)texels,
                                      // (ALLOW_UNORDERED_ACCESS: --compute exports into the texture)
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                      i == 0 ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET);
        cur_ = 0;
        tablesBuilt_ = false;
        return true;
    }

    bool frame(Material& mat, uint64_t tag, bool timeIt) override {
        prepare();
        UINT slot = (UINT)(frameCount_ % kSlots);
        dx_.waitFor(slotFence_[slot]);
        check(allocs_[slot]->Reset(), "allocator reset");
        check(cl_->Reset(allocs_[slot].Get(), nullptr), "list reset");
        timeIt = timeIt && tsHeap_ && !tsPending_;
        bool mixed = false;
        // --compute: the processor is the compute shader's (runCompute12) in place of the tick
        bool computing = computeMode && csCpu_ && mat.getFloat("_Init") == 0;
        // what computeImport12 takes back (captured before the zones below clear them)
        bool importCon = computing && conDelivered, importFetch = computing && deliver_;

        ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
        cl_->SetDescriptorHeaps(1, heaps);
        D3D12_VIEWPORT vp{0, 0, (float)kWidth, (float)kHeight, 0, 1};
        D3D12_RECT scissor{0, 0, (LONG)kWidth, (LONG)kHeight};
        cl_->RSSetViewports(1, &vp);
        cl_->RSSetScissorRects(1, &scissor);
        cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        int cur = cur_, dst = 1 - cur_;
        mat.setVector("CustomRenderTextureParameters", 1, 0, 0, 0);
        mat.setFloat("CustomRenderTexturePrimitiveIDs", 0);
        mat.setVector("_CustomRenderTextureInfo", kWidth, kHeight, 1, 0);

        auto draw = [&](int p, double cx, double cy, double zw, double zh, int cb = -1) {
            cl_->RSSetScissorRects(1, &scissor);
            Pass12& P = passes_[p];
            mat.setVector("CustomRenderTextureCenters", cx, cy, 0.5, 0);
            mat.setVector("CustomRenderTextureSizesAndRotations", zw, zh, 1, 0);
            mat.fillGlobals(P.vs);
            mat.fillGlobals(P.ps);
            if (P.hasGs) mat.fillGlobals(P.gs);
            // two a pass (the fetch zone's draw has the fourth pair), then the GPU draws, then the sound mix
            UINT64 base = ((UINT64)slot * kCbPerFrame + (UINT64)(cb < 0 ? p : cb) * 2) * kCbBytes;
            if (P.vs.hasGlobals) memcpy(cbMapped_ + base, P.vs.scratch.data(), P.vs.scratch.size());
            if (P.ps.hasGlobals) memcpy(cbMapped_ + base + kCbBytes, P.ps.scratch.data(), P.ps.scratch.size());
            cl_->SetPipelineState(P.pso.Get());
            cl_->SetGraphicsRootSignature(P.root.Get());
            cl_->SetGraphicsRootConstantBufferView(0, cbuf_->GetGPUVirtualAddress() + base);
            cl_->SetGraphicsRootConstantBufferView(1, cbuf_->GetGPUVirtualAddress() + base + kCbBytes);
            D3D12_GPU_DESCRIPTOR_HANDLE table = srvHeap_->GetGPUDescriptorHandleForHeapStart();
            table.ptr += (UINT64)tableIndex(p, cur) * srvStep_;
            cl_->SetGraphicsRootDescriptorTable(2, table);
            cl_->OMSetRenderTargets(1, &rtv_[dst], FALSE, nullptr);
            if (P.hasGs) {
                // the geometry shader amplifies each primitive: core 0's rect and the workers'
                // strips where the geometry puts them (docs/machine/multicore.md); it reads the state
                if (P.gsCb != 0xffffffff) {
                    UINT64 gsBase = ((UINT64)slot * kCbPerFrame + 18) * kCbBytes;
                    memcpy(cbMapped_ + gsBase, P.gs.scratch.data(), P.gs.scratch.size());
                    cl_->SetGraphicsRootConstantBufferView(P.gsCb, cbuf_->GetGPUVirtualAddress() + gsBase);
                }
                const D3D12_RESOURCE_STATES both =
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                auto toBoth = transition(state_[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, both);
                cl_->ResourceBarrier(1, &toBoth);
                cl_->DrawInstanced(3 * ((kCores + 15) / 16), 1, 0, 0);
                auto toPixel = transition(state_[cur].Get(), both, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                cl_->ResourceBarrier(1, &toPixel);
                return;
            }
            if (P.vsSelf < 0) {
                cl_->DrawInstanced(6, 1, 0, 0);
                return;
            }
            // the vertex shader picks the quads from the state: it needs the non-pixel state too
            D3D12_GPU_DESCRIPTOR_HANDLE self = srvHeap_->GetGPUDescriptorHandleForHeapStart();
            self.ptr += (UINT64)(8 * kTableSize + cur) * srvStep_;
            cl_->SetGraphicsRootDescriptorTable(P.vsTable, self);
            const D3D12_RESOURCE_STATES both =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            auto toBoth = transition(state_[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, both);
            cl_->ResourceBarrier(1, &toBoth);
            cl_->DrawInstanced(6 * kCommitQuads, 1, 0, 0);
            auto toPixel = transition(state_[cur].Get(), both, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cl_->ResourceBarrier(1, &toPixel);
        };
        auto stamp = [&](UINT i) {
            if (timeIt) cl_->EndQuery(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i);
        };

        // Zone 1: CPUTick on the 64x64 state area, then copy that area back so `cur` stays complete.
        // (--compute: the processor's passes and its export into state_[cur] instead; it is complete already)
        if (computing) {
            stamp(0);
            runCompute12(mat, slot, cur, dst);
            stamp(1);
            // (runCompute12 binds the compute heap: the draws below read theirs)
            ID3D12DescriptorHeap* gheap[] = {srvHeap_.Get()};
            cl_->SetDescriptorHeaps(1, gheap);
        } else {
        stamp(0);
        draw(0, 32, kHeight - (float)tickRows_ / 2, 64, (float)tickRows_);
        stamp(1);
        D3D12_RESOURCE_BARRIER toCopy[2] = {
            transition(state_[dst].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(state_[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
        cl_->ResourceBarrier(2, toCopy);
        D3D12_TEXTURE_COPY_LOCATION cdst{}, csrc{};
        cdst.pResource = state_[cur].Get();
        csrc.pResource = state_[dst].Get();
        D3D12_BOX box{0, 0, 0, 64, tickRows_, 1};   // what the pass drew
        cl_->CopyTextureRegion(&cdst, 0, 0, 0, &csrc, &box);
        D3D12_RESOURCE_BARRIER fromCopy[2] = {
            transition(state_[dst].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition(state_[cur].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
        cl_->ResourceBarrier(2, fromCopy);
        }

        // Zone 2: Commit on the whole texture; row 0 of the result goes to this slot's readback.
        // Commit reads the GPU's picture, to copy it into RAM when the guest asked.
        const D3D12_RESOURCE_STATES gpuRt = D3D12_RESOURCE_STATE_RENDER_TARGET, gpuRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (passCount_ == 3) {
            auto toRead = transition(gpuColor_.Get(), gpuRt, gpuRead);
            cl_->ResourceBarrier(1, &toRead);
        }
        draw(1, 1024, kHeight / 2.0f, 2048, (float)kHeight);
        if (passCount_ == 3) {
            auto toDraw = transition(gpuColor_.Get(), gpuRead, gpuRt);
            cl_->ResourceBarrier(1, &toDraw);
        }
        stamp(2);
        // The GPU device's zone reads the committed state and writes part of it: it renders
        // into the other buffer and its rows are copied back, as the CPUTick zone does.
        if (passCount_ == 3) {
            D3D12_RESOURCE_BARRIER swapIn[2] = {
                transition(state_[dst].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                transition(state_[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)};
            cl_->ResourceBarrier(2, swapIn);
            std::swap(cur, dst);   // `cur` is the committed state from here on
            // The mesh, into the GPU's own target. Depth starts fresh every frame; colour is
            // kept, so the picture stays until the guest submits another list.
            D3D12_CPU_DESCRIPTOR_HANDLE grtv = gpuRtvHeap_->GetCPUDescriptorHandleForHeapStart();
            D3D12_CPU_DESCRIPTOR_HANDLE gdsv = gpuDsvHeap_->GetCPUDescriptorHandleForHeapStart();
            D3D12_VIEWPORT gvp{0, 0, (float)kGpuTarget, (float)kGpuTarget, 0, 1};
            D3D12_RECT gsc{0, 0, (LONG)kGpuTarget, (LONG)kGpuTarget};
            cl_->RSSetViewports(1, &gvp);
            cl_->RSSetScissorRects(1, &gsc);
            cl_->OMSetRenderTargets(1, &grtv, FALSE, &gdsv);
            cl_->ClearDepthStencilView(gdsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            cl_->SetGraphicsRootSignature(gpuRoot_.Get());
            D3D12_GPU_DESCRIPTOR_HANDLE gtable = srvHeap_->GetGPUDescriptorHandleForHeapStart();
            gtable.ptr += (UINT64)(8 * kTableSize + 2 + cur * kGpuTable) * srvStep_;
            cl_->SetGraphicsRootDescriptorTable(0, gtable);
            // the vertex shader reads the state texture too, which needs the non-pixel state
            const D3D12_RESOURCE_STATES anyStage =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            auto toAny = transition(state_[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, anyStage);
            cl_->ResourceBarrier(1, &toAny);
            // one draw per pass in use, in order: each has its own blending and depth use
            for (int p = 0; p < 8; ++p) {
                if (!((gpuPasses >> p) & 1)) continue;
                if (gpuVl_.hasGlobals) {
                    mat.setInt("_GpuPass", p);
                    mat.fillGlobals(gpuVl_);
                    UINT64 at = ((UINT64)slot * kCbPerFrame + 8 + (UINT64)p) * kCbBytes;
                    memcpy(cbMapped_ + at, gpuVl_.scratch.data(), gpuVl_.scratch.size());
                    cl_->SetGraphicsRootConstantBufferView(1, cbuf_->GetGPUVirtualAddress() + at);
                }
                cl_->SetPipelineState(gpuPso_[p].Get());
                cl_->DrawInstanced(kGpuTriangles * 3, 1, 0, 0);
            }
            auto toPixel = transition(state_[cur].Get(), anyStage, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cl_->ResourceBarrier(1, &toPixel);
            // The sound card's mix, from the committed state, before the control zone moves the
            // voices on; it shares the GPU draw's table (the state and the ROM).
            mixed = soundMix && soundPso_;
            if (mixed) {
                D3D12_CPU_DESCRIPTOR_HANDLE srtv = soundRtvHeap_->GetCPUDescriptorHandleForHeapStart();
                D3D12_VIEWPORT svp{0, 0, (float)kSoundSide, (float)kSoundSide, 0, 1};
                D3D12_RECT ssc{0, 0, (LONG)kSoundSide, (LONG)kSoundSide};
                cl_->RSSetViewports(1, &svp);
                cl_->RSSetScissorRects(1, &ssc);
                cl_->OMSetRenderTargets(1, &srtv, FALSE, nullptr);
                cl_->SetGraphicsRootSignature(soundRoot_.Get());
                cl_->SetGraphicsRootDescriptorTable(0, gtable);
                if (soundPl_.hasGlobals) {
                    mat.fillGlobals(soundPl_);
                    UINT64 at = ((UINT64)slot * kCbPerFrame + 16) * kCbBytes;
                    memcpy(cbMapped_ + at, soundPl_.scratch.data(), soundPl_.scratch.size());
                    cl_->SetGraphicsRootConstantBufferView(1, cbuf_->GetGPUVirtualAddress() + at);
                }
                cl_->SetPipelineState(soundPso_.Get());
                cl_->DrawInstanced(3, 1, 0, 0);
                auto toCopy = transition(soundTex_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                cl_->ResourceBarrier(1, &toCopy);
                D3D12_TEXTURE_COPY_LOCATION sd{}, ss{};
                sd.pResource = soundBuf_[slot].Get();
                sd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                sd.PlacedFootprint = soundFootprint_;
                ss.pResource = soundTex_.Get();
                cl_->CopyTextureRegion(&sd, 0, 0, 0, &ss, nullptr);
                auto toDraw = transition(soundTex_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                cl_->ResourceBarrier(1, &toDraw);
            }
        cl_->RSSetViewports(1, &vp);
        // then the control zone on the state texture: mark the list drawn, deliver input
            for (int z = 0; z < 3; ++z) {
                if ((z == 1 && !conDelivered) || (z == 2 && !deliver_)) continue;
                const float* zn = z == 0 ? kGpuControlZone : z == 1 ? kConZone : kFetchZone;
                draw(2, zn[0], zn[1], zn[2], zn[3], z < 2 ? -1 : 3);
                D3D12_RESOURCE_BARRIER in[2] = {
                    transition(state_[dst].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
                    transition(state_[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
                cl_->ResourceBarrier(2, in);
                UINT top = kHeight - (UINT)(zn[1] + zn[3] / 2), left = (UINT)(zn[0] - zn[2] / 2);
                D3D12_TEXTURE_COPY_LOCATION zd{}, zs{};
                zd.pResource = state_[cur].Get();
                zs.pResource = state_[dst].Get();
                D3D12_BOX zbox{left, top, 0, left + (UINT)zn[2], top + (UINT)zn[3], 1};
                cl_->CopyTextureRegion(&zd, left, top, 0, &zs, &zbox);
                D3D12_RESOURCE_BARRIER out[2] = {
                    transition(state_[dst].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                    transition(state_[cur].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
                cl_->ResourceBarrier(2, out);
            }
            deliver_ = false;
            conDelivered = false;
            // back to the names the rest of the frame uses: dst = the new state, in RENDER_TARGET
            D3D12_RESOURCE_BARRIER swapOut[2] = {
                transition(state_[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                transition(state_[dst].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
            cl_->ResourceBarrier(2, swapOut);
            std::swap(cur, dst);
        }
        stamp(3);
        if (timeIt) cl_->ResolveQueryData(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, tsBuf_.Get(), 0);
        // --compute: what the devices wrote, back into the processor's (its texture is the committed one)
        // (no network ring or counters' merge on this backend: as its fragment path has none)
        if (computing) computeImport12(importCon, importFetch, false, false, slot, dst);
        // --compute: every core's state for its instruction count (the tick keeps it, sync_cores need not run)
        if (computing && cpuState_) {
            auto toStats = transitionBuf(cpuState_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cl_->ResourceBarrier(1, &toStats);
            cl_->CopyBufferRegion(statBuf_[slot].Get(), 0, cpuState_.Get(), 0, (UINT64)kCoresMost * kCpuStateBytes);
            auto toUav = transitionBuf(cpuState_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl_->ResourceBarrier(1, &toUav);
        }
        auto toRead = transition(state_[dst].Get(), computing ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET,
                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl_->ResourceBarrier(1, &toRead);
        D3D12_TEXTURE_COPY_LOCATION rdst{}, rsrc{};
        rdst.pResource = rowBuf_[slot].Get();
        rdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rdst.PlacedFootprint = rowFootprint_;
        rsrc.pResource = state_[dst].Get();
        D3D12_BOX rowBox{0, 0, 0, 64, 1, 1};
        cl_->CopyTextureRegion(&rdst, 0, 0, 0, &rsrc, &rowBox);
        D3D12_BOX controlBox{0, kControlRow, 0, kControlTexels, kControlRow + 1, 1};
        cl_->CopyTextureRegion(&rdst, 0, 1, 0, &rsrc, &controlBox);
        D3D12_BOX netBox{0, kNetRow, 0, 64, kNetRow + 1, 1};
        cl_->CopyTextureRegion(&rdst, 0, 2, 0, &rsrc, &netBox);
        if (zoneBuf_[slot]) {
            D3D12_TEXTURE_COPY_LOCATION zdst = rdst;
            zdst.pResource = zoneBuf_[slot].Get();
            zdst.PlacedFootprint = zoneFootprint_;
            D3D12_BOX zoneBox{0, 0, 0, 64, 64, 1};
            cl_->CopyTextureRegion(&zdst, 0, 0, 0, &rsrc, &zoneBox);
        }
        D3D12_RESOURCE_BARRIER swapStates[2] = {
            transition(state_[dst].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            transition(state_[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)};
        cl_->ResourceBarrier(2, swapStates);
        checkDev(dx_, cl_->Close(), "Close");
        ID3D12CommandList* lists[] = {cl_.Get()};
        dx_.queue->ExecuteCommandLists(1, lists);
        slotFence_[slot] = dx_.signal();
        if (timeIt) {
            tsPending_ = true;
            tsFence_ = slotFence_[slot];
        }
        rows_.push_back({slot, slotFence_[slot], tag, mixed});
        cur_ = dst;
        ++frameCount_;
        return true;
    }
    bool rowFull() const override { return rows_.size() >= kSlots; }
    size_t rowPending() const override { return rows_.size(); }
    bool popRow(std::vector<uint8_t>& out, uint64_t& tag) override {
        if (rows_.empty()) return false;
        PendingRow r = rows_.front();
        rows_.pop_front();
        dx_.waitFor(r.fence);
        if (FAILED(dx_.dev->GetDeviceRemovedReason())) return false;
        uint8_t* p = nullptr;
        if (FAILED(rowBuf_[r.slot]->Map(0, nullptr, (void**)&p))) return false;
        const uint8_t* base = p + rowFootprint_.Offset;
        out.assign(base, base + 64 * 16);
        out.insert(out.end(), base + rowFootprint_.Footprint.RowPitch, base + rowFootprint_.Footprint.RowPitch + kControlTexels * 16);
        out.insert(out.end(), base + 2 * rowFootprint_.Footprint.RowPitch, base + 2 * rowFootprint_.Footprint.RowPitch + 64 * 16);
        rowBuf_[r.slot]->Unmap(0, nullptr);
        tag = r.tag;
        lastZone_.clear();
        if (zoneBuf_[r.slot] && SUCCEEDED(zoneBuf_[r.slot]->Map(0, nullptr, (void**)&p))) {
            lastZone_.resize(64 * 64 * 16);
            for (UINT y = 0; y < 64; ++y)
                memcpy(lastZone_.data() + y * 64 * 16, p + zoneFootprint_.Offset + (size_t)y * zoneFootprint_.Footprint.RowPitch, 64 * 16);
            zoneBuf_[r.slot]->Unmap(0, nullptr);
        }
        lastSound_.clear();
        if (r.sound && SUCCEEDED(soundBuf_[r.slot]->Map(0, nullptr, (void**)&p))) {
            const size_t pitch = (size_t)kSoundSide * 8;
            lastSound_.resize(pitch * kSoundSide);
            for (UINT y = 0; y < kSoundSide; ++y)
                memcpy(lastSound_.data() + y * pitch, p + soundFootprint_.Offset + (size_t)y * soundFootprint_.Footprint.RowPitch, pitch);
            soundBuf_[r.slot]->Unmap(0, nullptr);
        }
        lastStats_.assign(256, 0);
        if (statBuf_[r.slot] && SUCCEEDED(statBuf_[r.slot]->Map(0, nullptr, (void**)&p))) {
            const uint32_t* w = (const uint32_t*)p;
            for (int c = 0; c < 256; ++c)
                lastStats_[c] = (uint64_t)w[(size_t)c * 128 + 93] | ((uint64_t)w[(size_t)c * 128 + 94] << 32);
            // the CPU State panel's decoded cores (words per core: r64, csr16, pc/priv/..., instr, mode)
            static MemoryViewCore cores[256];
            for (int c = 0; c < 256; ++c) {
                const uint32_t* s = w + (size_t)c * 128;
                MemoryViewCore& k = cores[c];
                for (int i = 0; i < 64; ++i) k.r[i] = s[i];
                for (int i = 0; i < 16; ++i) k.csr[i] = s[64 + i];
                k.pc = s[80];
                k.priv = s[81];
                k.tcmpLo = s[84];
                k.tcmpHi = s[85];
                k.instr = lastStats_[(size_t)c];
                k.mode = s[98];
                k.seen = s[99];
                k.job = s[101];
            }
            memoryViewSetCores(cores, 256);
            statBuf_[r.slot]->Unmap(0, nullptr);
        }
        return true;
    }
    // Every core's instruction count, as of the last popped frame (64-bit each).
    bool computeWorkerCounts(std::vector<uint64_t>& out) override {
        if (lastStats_.empty()) return false;
        out = lastStats_;
        return true;
    }
    bool takeState(std::vector<uint8_t>& out) override {
        if (lastZone_.empty()) return false;
        out.swap(lastZone_);
        lastZone_.clear();
        return true;
    }
    bool takeSound(std::vector<uint8_t>& out) override {
        if (lastSound_.empty()) return false;
        out.swap(lastSound_);
        lastSound_.clear();
        return true;
    }

    bool computeStart(const std::string& imageDir, std::string& err) override;
    bool computeState(std::vector<uint32_t>& out) override;

    bool readState(UINT w, UINT h, std::vector<uint8_t>& out) override {
        prepare();
        dx_.flush();
        out = readRegion(dx_, state_[cur_].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, w, h);
        return true;
    }

    bool gpuTimes(double& tickMs, double& commitMs, double& deviceMs) override {
        UINT64 freq = 0;
        if (tsPending_ && dx_.fence->GetCompletedValue() >= tsFence_) {
            UINT64* ts = nullptr;
            if (SUCCEEDED(dx_.queue->GetTimestampFrequency(&freq)) && freq && SUCCEEDED(tsBuf_->Map(0, nullptr, (void**)&ts))) {
                tickMs_ = (double)(ts[1] - ts[0]) * 1000.0 / (double)freq;
                commitMs_ = (double)(ts[2] - ts[1]) * 1000.0 / (double)freq;
                deviceMs_ = (double)(ts[3] - ts[2]) * 1000.0 / (double)freq;
                tsBuf_->Unmap(0, nullptr);
            }
            tsPending_ = false;
        }
        tickMs = tickMs_;
        commitMs = commitMs_;
        deviceMs = deviceMs_;
        return tickMs_ >= 0;
    }

    std::string deviceRemoved() override {
        HRESULT hr = dx_.dev->GetDeviceRemovedReason();
        return FAILED(hr) ? hrToString(hr) : std::string();
    }

    bool viewInit(std::string& err) override { return view_.init(dx_, kWidth, kHeight, 64, err); }
    bool viewOpen() const override { return view_.open(); }
    void viewRender(bool present) override {
        if (!view_.open()) return;
        prepare();
        dx_.flush();
        view_.render(state_[cur_].Get(), present, gpuColor_.Get());
    }
    bool gpuCapture(const std::string& path) override {
        if (!gpuColor_) return false;
        dx_.flush();
        auto td = texDesc(kGpuTarget, kGpuTarget, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        dx_.dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
        ComPtr<ID3D12Resource> rb;
        auto bd = bufferDesc(total);
        auto hr = heapProps(D3D12_HEAP_TYPE_READBACK);
        check(dx_.dev->CreateCommittedResource(&hr, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&rb)), "readback buffer");
        OneShot os(dx_);
        auto b0 = transition(gpuColor_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        os.list->ResourceBarrier(1, &b0);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = rb.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = gpuColor_.Get();
        os.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        auto b1 = transition(gpuColor_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        os.list->ResourceBarrier(1, &b1);
        os.run();
        uint8_t* p = nullptr;
        if (FAILED(rb->Map(0, nullptr, (void**)&p))) return false;
        bool ok = memoryViewWriteBmp(path, kGpuTarget, kGpuTarget, p + fp.Offset, fp.Footprint.RowPitch);
        rb->Unmap(0, nullptr);
        return ok;
    }
    void viewText(const std::vector<std::vector<std::string>>& columns) override { view_.setText(columns); }
    void viewTitle(const std::string& title) override { view_.setTitle(title); }
    bool viewCapture(const std::string& path) override { return view_.capture(path); }
    void viewClose() override { view_.close(); }

private:
    struct PendingRow {
        UINT slot;
        UINT64 fence;
        uint64_t tag;
        bool sound;   // the frame drew the sound card's mix into its slot's buffer
    };

    static UINT tableIndex(int pass, int cur) { return (UINT)(pass * 2 + cur) * kTableSize; }

    ComPtr<ID3D12Resource> readbackBuffer(D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, UINT w, UINT h, DXGI_FORMAT format) {
        auto td = texDesc(w, h, format, D3D12_RESOURCE_FLAG_NONE);
        UINT64 total = 0;
        dx_.dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
        ComPtr<ID3D12Resource> rb;
        auto bd = bufferDesc(total);
        auto hr = heapProps(D3D12_HEAP_TYPE_READBACK);
        check(dx_.dev->CreateCommittedResource(&hr, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&rb)), "readback buffer");
        return rb;
    }

    // State textures (an empty machine unless setState ran) and the views that depend on them
    // and on the payload textures: RTV per state texture, one SRV table per (pass, current).
    void prepare() {
        if (tablesBuilt_) return;
        std::string err;
        if (!state_[0]) setState(nullptr, err);
        dx_.flush();
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 2;
        check(dx_.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_)), "RTV heap");
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        // the tables, each state texture alone, then the GPU draw's tables (state and ROM)
        hd.NumDescriptors = 8 * kTableSize + 2 + 2 * kGpuTable;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(dx_.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap_)), "SRV heap");
        UINT rtvStep = dx_.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srvStep_ = dx_.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (int i = 0; i < 2; ++i) {
            rtv_[i] = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            rtv_[i].ptr += (SIZE_T)i * rtvStep;
            dx_.dev->CreateRenderTargetView(state_[i].Get(), nullptr, rtv_[i]);
        }
        for (int p = 0; p < passCount_; ++p) {
            for (int cur = 0; cur < 2; ++cur) {
                for (UINT s = 0; s < kTableSize; ++s) {
                    ID3D12Resource* res = blackTex_.Get();
                    for (auto& t : passes_[p].ps.textures) {
                        if (t.slot != s) continue;
                        if (t.name == "_SelfTexture2D") res = state_[cur].Get();
                        else if (textures_.count(t.name)) res = textures_[t.name].Get();
                    }
                    D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
                    h.ptr += (SIZE_T)(tableIndex(p, cur) + s) * srvStep_;
                    dx_.dev->CreateShaderResourceView(res, nullptr, h);
                }
            }
        }
        for (int i = 0; i < 2; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)(8 * kTableSize + i) * srvStep_;
            dx_.dev->CreateShaderResourceView(state_[i].Get(), nullptr, h);
        }
        for (int cur = 0; cur < 2; ++cur) {
            for (UINT s = 0; s < kGpuTable; ++s) {
                ID3D12Resource* res = s == 0 ? state_[cur].Get() : blackTex_.Get();
                for (auto& t : gpuPl_.textures)
                    if (t.slot == s && textures_.count(t.name)) res = textures_[t.name].Get();
                D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
                h.ptr += (SIZE_T)(8 * kTableSize + 2 + cur * kGpuTable + s) * srvStep_;
                dx_.dev->CreateShaderResourceView(res, nullptr, h);
            }
        }
        tablesBuilt_ = true;
    }

    Dx dx_;
    Pass12 passes_[3];   // CPUTick, Commit, and the GPU device's control zone if present
    int passCount_ = 2;

    // The GPU device's draw: its own pipeline (depth test, state texture visible to both stages)
    // and its own colour and depth target.
    void buildGpuDraw(const SLShader& shader, const CompileSettings& cs, const Build12& b) {
        const SLPass* sp = shader.findPass("GPUDraw");
        if (!sp) die("gpu.shader has no GPUDraw pass");
        Dxc dxc;
        dxc.init(b.dxcDir);
        std::string err, vsText, psText, rootDir = fs::u8path(shader.path).parent_path().u8string();
        if (!preprocessStage(sp->code, shader.path, rootDir, {{"SHADER_STAGE_VERTEX", "1"}}, cs, vsText, err) ||
            !preprocessStage(sp->code, shader.path, rootDir, {{"SHADER_STAGE_FRAGMENT", "1"}}, cs, psText, err))
            die(err.c_str());
        StageLayout &vl = gpuVl_, &pl = gpuPl_;
        std::vector<uint8_t> vs = dxcCompile(dxc, vsText, sp->vertexEntry, "vs_" + b.dxcSm, b.dxcOpt, vl);
        std::vector<uint8_t> ps = dxcCompile(dxc, psText, sp->fragmentEntry, "ps_" + b.dxcSm, b.dxcOpt, pl);
        for (auto& t : vl.textures)
            if ((t.slot == 0) != (t.name == "_State") || t.slot >= kGpuTable) die("GPUDraw: the state texture must be t0, the ROM t1-t4");
        for (auto& t : pl.textures)
            if ((t.slot == 0) != (t.name == "_State") || t.slot >= kGpuTable) die("GPUDraw: the state texture must be t0, the ROM t1-t4");
        if (pl.hasGlobals) die("GPUDraw: uniforms in the vertex shader only");
        if (vl.globalsSize > kCbBytes) die("GPUDraw: $Globals larger than the upload slot");

        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = kGpuTable;
        // [0] the state texture for both stages and the ROM, [1] the vertex shader's uniforms
        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges = 1;
        rp[0].DescriptorTable.pDescriptorRanges = &range;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp[1].Descriptor.ShaderRegister = vl.globalsSlot;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = vl.hasGlobals ? 2 : 1;
        rsd.pParameters = rp;
        ComPtr<ID3DBlob> blob, rsErr;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &rsErr))) {
            fprintf(stderr, "[d3d12] GPU root signature (uniforms: slot %u, %u bytes): %s\n", vl.globalsSlot, vl.globalsSize,
                    rsErr ? (const char*)rsErr->GetBufferPointer() : "?");
            die("GPU root signature");
        }
        check(dx_.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&gpuRoot_)), "GPU root signature");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = gpuRoot_.Get();
        pd.VS = {vs.data(), vs.size()};
        pd.PS = {ps.data(), ps.size()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xffffffff;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        // The eight passes (FRAGMENT_PASS in gpu.h): opaque, alpha, additive, multiply with the
        // depth test, then the same four without. Only the first writes depth.
        for (int p = 0; p < 8; ++p) {
            auto& rt = pd.BlendState.RenderTarget[0];
            int blend = p & 3;
            rt.BlendEnable = blend != 0;
            rt.SrcBlend = rt.SrcBlendAlpha = blend == 3 ? D3D12_BLEND_DEST_COLOR : D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend = rt.DestBlendAlpha = blend == 1 ? D3D12_BLEND_INV_SRC_ALPHA : blend == 2 ? D3D12_BLEND_ONE : D3D12_BLEND_ZERO;
            rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            if (blend == 3) rt.SrcBlendAlpha = D3D12_BLEND_ZERO, rt.DestBlendAlpha = D3D12_BLEND_ONE;
            pd.DepthStencilState.DepthEnable = p < 4;
            pd.DepthStencilState.DepthWriteMask = p == 0 ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            check(dx_.dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&gpuPso_[p])), "GPU pipeline state");
        }
        fprintf(stderr, "[d3d12] pass 'GPUDraw': vs %zu bytes, ps %zu bytes\n", vs.size(), ps.size());

        auto hp = heapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto cd = texDesc(kGpuTarget, kGpuTarget, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto dd = texDesc(kGpuTarget, kGpuTarget, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = DXGI_FORMAT_D32_FLOAT;
        depthClear.DepthStencil.Depth = 1.0f;
        check(dx_.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                               IID_PPV_ARGS(&gpuColor_)), "GPU colour target");
        check(dx_.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                                               IID_PPV_ARGS(&gpuDepth_)), "GPU depth target");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        check(dx_.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gpuRtvHeap_)), "GPU RTV heap");
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        check(dx_.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gpuDsvHeap_)), "GPU DSV heap");
        dx_.dev->CreateRenderTargetView(gpuColor_.Get(), nullptr, gpuRtvHeap_->GetCPUDescriptorHandleForHeapStart());
        dx_.dev->CreateDepthStencilView(gpuDepth_.Get(), nullptr, gpuDsvHeap_->GetCPUDescriptorHandleForHeapStart());
        OneShot os(dx_);
        const float black[4] = {0, 0, 0, 1};
        os.list->ClearRenderTargetView(gpuRtvHeap_->GetCPUDescriptorHandleForHeapStart(), black, 0, nullptr);
        os.run();
    }

    // The sound card's mix: a pixel shader over its own small target, reading what the GPU
    // draw reads (the state at t0, the ROM at t1-t4).
    void buildSoundMix(const SLShader& shader, const CompileSettings& cs, const Build12& b) {
        const SLPass* sp = shader.findPass("SoundMix");
        if (!sp) die("sound.shader has no SoundMix pass");
        Dxc dxc;
        dxc.init(b.dxcDir);
        std::string err, vsText, psText, rootDir = fs::u8path(shader.path).parent_path().u8string();
        if (!preprocessStage(sp->code, shader.path, rootDir, {{"SHADER_STAGE_VERTEX", "1"}}, cs, vsText, err) ||
            !preprocessStage(sp->code, shader.path, rootDir, {{"SHADER_STAGE_FRAGMENT", "1"}}, cs, psText, err))
            die(err.c_str());
        StageLayout vl;
        StageLayout& pl = soundPl_;
        std::vector<uint8_t> vs = dxcCompile(dxc, vsText, sp->vertexEntry, "vs_" + b.dxcSm, b.dxcOpt, vl);
        std::vector<uint8_t> ps = dxcCompile(dxc, psText, sp->fragmentEntry, "ps_" + b.dxcSm, b.dxcOpt, pl);
        if (!vl.textures.empty() || vl.hasGlobals) die("SoundMix: the vertex shader reads nothing");
        for (auto& t : pl.textures)
            if ((t.slot == 0) != (t.name == "_State") || t.slot >= kGpuTable) die("SoundMix: the state texture must be t0, the ROM t1-t4");
        for (auto& t : pl.textures)
            for (auto& g : gpuPl_.textures)
                if (g.slot == t.slot && g.name != t.name) die("SoundMix: its textures must sit where GPUDraw's do");
        if (pl.globalsSize > kCbBytes) die("SoundMix: $Globals larger than the upload slot");

        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = kGpuTable;
        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges = 1;
        rp[0].DescriptorTable.pDescriptorRanges = &range;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp[1].Descriptor.ShaderRegister = pl.globalsSlot;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = pl.hasGlobals ? 2 : 1;
        rsd.pParameters = rp;
        ComPtr<ID3DBlob> blob, rsErr;
        check(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &rsErr), "sound root signature");
        check(dx_.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&soundRoot_)), "sound root signature");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = soundRoot_.Get();
        pd.VS = {vs.data(), vs.size()};
        pd.PS = {ps.data(), ps.size()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xffffffff;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = kSoundFormat;
        pd.SampleDesc.Count = 1;
        check(dx_.dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&soundPso_)), "sound pipeline state");
        fprintf(stderr, "[d3d12] pass 'SoundMix': ps %zu bytes\n", ps.size());

        auto hp = heapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto td = texDesc(kSoundSide, kSoundSide, kSoundFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        check(dx_.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                               IID_PPV_ARGS(&soundTex_)), "sound target");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        check(dx_.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&soundRtvHeap_)), "sound RTV heap");
        dx_.dev->CreateRenderTargetView(soundTex_.Get(), nullptr, soundRtvHeap_->GetCPUDescriptorHandleForHeapStart());
    }

    static constexpr DXGI_FORMAT kSoundFormat = DXGI_FORMAT_R32G32_FLOAT;
    static constexpr UINT kCbPerFrame = 20;   // constant-buffer slots a frame
    ComPtr<ID3D12RootSignature> soundRoot_;
    ComPtr<ID3D12PipelineState> soundPso_;
    StageLayout soundPl_;
    ComPtr<ID3D12Resource> soundTex_, soundBuf_[kSlots];
    ComPtr<ID3D12DescriptorHeap> soundRtvHeap_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT soundFootprint_{};
    std::vector<uint8_t> lastSound_;
    ComPtr<ID3D12Resource> zoneBuf_[kSlots];   // --l1-log: the CPU's state texels of each frame
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT zoneFootprint_{};
    std::vector<uint8_t> lastZone_;

    // --compute: the processor as compute shaders (experiments/rv32_min), RAM in buffers of
    // its own (rvc_backend.h). Mirrors rvc_backend11's compute path.
    static const UINT kCpuStateBytes = 512;   // a core's words
    static const UINT kMarkBytes = (3 * 73728 + 128 + 1024 + 32768) * 4;   // page marks and lists
    static const UINT kArgsBytes = 48;   // merge's dispatch, export_pages's, the next pass's
    static const UINT64 kRamBytes = (UINT64)2048 * (kStateHeight - 64) * 16;   // 288 MB
    ComPtr<ID3D12RootSignature> csTickRoot_, csRoot_;
    ComPtr<ID3D12PipelineState> csCpu_, csMerge_, csMergeList_, csCount_, csState_, csExport_, csPlan_, csTake_, csCores_, csExportPages_, csRadiate_, csPaintTex_;
    ComPtr<ID3D12Resource> cpuRam_, cpuState_, cpuRom_, cpuDec_, cpuMarks_, cpuArgs_;
    uint32_t syncCb_[8] = {};   // the sync pass's current constants (b0), bound before each dispatch
    uint32_t radCb_[12] = {};   // radiate's constants (b0): its own twelve, set all at once
    uint32_t radiateSeed_ = 0x51ed2703u;
    uint32_t radiatePaint_ = ~0u;   // paint_tex's texel after the export (this frame's radiate)
    uint32_t radiatePaintVal_ = 0;
    int radiateFlips_ = 0;   // full export mask while flipping (the bands show at once)
    ComPtr<ID3D12Resource> cpuStateRead_;   // READBACK for computeState
    ComPtr<ID3D12DescriptorHeap> csHeap_[kSlots];   // per-slot: 6 UAV + 3 SRV
    ComPtr<ID3D12CommandSignature> csIndirect_;
    UINT csStep_ = 0;   // this heap's descriptor size
    uint32_t cpuSeed_ = 0x9e3779b9u, computeFrames_ = 0;
    bool buildComputeCpu(const std::string& path, const BackendOptions& opt, std::string& err);
    void runCompute12(Material& mat, UINT slot, int cur, int dst);
    void computeImport12(bool con, bool fetch, bool net, bool counts, UINT slot, int dst);
    void uavBarriers(UINT n, ID3D12Resource* const* resources);
    void setSync12(uint32_t mask, uint32_t mode, uint32_t from, uint32_t count);
    void setRadiate12(uint32_t flips, uint32_t r0s, uint32_t r0e, uint32_t r1s, uint32_t r1e,
                      uint32_t ps, uint32_t pe, uint32_t paint, uint32_t paintVal);

    ComPtr<ID3D12RootSignature> gpuRoot_;
    ComPtr<ID3D12PipelineState> gpuPso_[8];
    StageLayout gpuVl_, gpuPl_;
    static constexpr UINT kGpuTable = 5;   // the GPU draw's textures: the state, then the ROM's four
    ComPtr<ID3D12Resource> gpuColor_, gpuDepth_;
    ComPtr<ID3D12DescriptorHeap> gpuRtvHeap_, gpuDsvHeap_;
    ComPtr<ID3D12Resource> state_[2], blackTex_;
    std::map<std::string, ComPtr<ID3D12Resource>> textures_;
    int cur_ = 0;
    bool tablesBuilt_ = false;
    bool deliver_ = false;   // the fetch zone is drawn in the next frame
    ComPtr<ID3D12DescriptorHeap> rtvHeap_, srvHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_[2]{};
    UINT tickRows_ = 64;   // BackendOptions::tickRows
    UINT srvStep_ = 0;
    ComPtr<ID3D12CommandAllocator> allocs_[kSlots];
    UINT64 slotFence_[kSlots] = {};
    ComPtr<ID3D12GraphicsCommandList> cl_;
    ComPtr<ID3D12Resource> cbuf_;
    uint8_t* cbMapped_ = nullptr;
    uint64_t frameCount_ = 0;
    ComPtr<ID3D12Resource> rowBuf_[kSlots];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT rowFootprint_{};
    std::deque<PendingRow> rows_;
    ComPtr<ID3D12Resource> statBuf_[kSlots];   // the cores' state, for their instruction counts
    std::vector<uint64_t> lastStats_;          // as of the last popped frame (256 counts, core 0 first)
    ComPtr<ID3D12QueryHeap> tsHeap_;
    ComPtr<ID3D12Resource> tsBuf_;
    bool tsPending_ = false;
    UINT64 tsFence_ = 0;
    double tickMs_ = -1, commitMs_ = -1, deviceMs_ = 0;

    MemoryView12 view_;
};

// --compute (experiments/rv32_min): DXC-compiled compute pipelines and the root signature they
// share. One root signature for all entries: [0] a UAV table u0..u5 (Ram, State, Dec, Marks,
// Args and the state texture for export), [1] an SRV table t0..t1 (the ROM, the state texture
// for import and a spare), [2] the pass/sync constants as a root-descriptor CBV (b0).
bool Backend12::buildComputeCpu(const std::string& path, const BackendOptions& opt, std::string& err) {
    fs::path cpuPath = fs::u8path(path), syncPath = cpuPath.parent_path() / "sync.hlsl";
    if (!fs::exists(syncPath)) syncPath = fs::u8path("experiments/rv32_min/sync.hlsl");
    std::string cpuSource, syncSource;
    if (!readFileBinary(cpuPath.u8string(), cpuSource)) { err = "--compute: no shader " + path; return false; }
    if (!readFileBinary(syncPath.u8string(), syncSource)) { err = "--compute: no " + syncPath.u8string(); return false; }
    Dxc dxc;
    dxc.init(opt.dxcDir);
    // Two root signatures: the tick's u0-u4 (Ram, State, Dec, Marks, Args) and
    // sync.hlsl's u0-u5 (Ram, State, the export's texture, Dec, Marks, Args): u2 is Dec in
    // one shader and the texture in the other, so one table cannot serve both. The SRV
    // table (the ROM, the committed texture and a spare) and the constants (b0) are shared.
    auto makeRoot = [&](D3D12_DESCRIPTOR_RANGE& ur, const char* what) {
        D3D12_DESCRIPTOR_RANGE sr{};
        sr.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sr.NumDescriptors = 2;
        sr.BaseShaderRegister = 0;
        sr.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER rp[3]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges = 1;
        rp[0].DescriptorTable.pDescriptorRanges = &ur;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges = &sr;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // the pass/sync constants (b0) as root constants: each dispatch's values are in the
        // command list itself (an upload buffer would hold only the last pass's when the GPU runs them)
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[2].Constants.ShaderRegister = 0;
        rp[2].Constants.RegisterSpace = 0;
        rp[2].Constants.Num32BitValues = 12;
        rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 3;
        rsd.pParameters = rp;
        ComPtr<ID3DBlob> rsBlob, rsErr;
        check(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr), what);
        ComPtr<ID3D12RootSignature> root;
        check(dx_.dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&root)), what);
        return root;
    };
    D3D12_DESCRIPTOR_RANGE urTick{};
    urTick.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    urTick.NumDescriptors = 5;
    urTick.BaseShaderRegister = 0;
    urTick.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE urSync{};
    urSync.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    urSync.NumDescriptors = 6;
    urSync.BaseShaderRegister = 0;
    urSync.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    csTickRoot_ = makeRoot(urTick, "compute tick root signature");
    csRoot_ = makeRoot(urSync, "compute sync root signature");
    D3D12_INDIRECT_ARGUMENT_DESC iarg{};
    iarg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC sd{};
    sd.ByteStride = 12;
    sd.NumArgumentDescs = 1;
    sd.pArgumentDescs = &iarg;
    // (a root signature only if the signature changes root arguments: Dispatch alone takes none)
    check(dx_.dev->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&csIndirect_)), "compute indirect signature");

    struct Entry { const std::string* source; std::string file; const char* entry; ComPtr<ID3D12PipelineState>* pso; };
    Entry entries[] = {{&cpuSource, cpuPath.u8string(), "tick", &csCpu_}, {&syncSource, syncPath.u8string(), "sync_count", &csCount_},
                       {&syncSource, syncPath.u8string(), "sync_state", &csState_}, {&syncSource, syncPath.u8string(), "export_rows", &csExport_},
                       {&syncSource, syncPath.u8string(), "import_plan", &csPlan_}, {&syncSource, syncPath.u8string(), "import_rows", &csTake_},
                       {&syncSource, syncPath.u8string(), "sync_cores", &csCores_}, {&syncSource, syncPath.u8string(), "export_pages", &csExportPages_},
                       {&syncSource, syncPath.u8string(), "radiate", &csRadiate_}, {&syncSource, syncPath.u8string(), "paint_tex", &csPaintTex_}};
    if (cpuSource.find("void merge(") != std::string::npos) {
        for (const char* entry : {"merge", "merge_list"}) {
            StageLayout dummy;
            std::vector<uint8_t> code = dxcCompile(dxc, cpuSource, entry, "cs_" + opt.dxcSm, opt.dxcOpt, dummy);
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = csTickRoot_.Get();
            pd.CS = {code.data(), code.size()};
            auto t0 = std::chrono::steady_clock::now();
            bool isMerge = strcmp(entry, "merge") == 0;
            check(dx_.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(isMerge ? csMerge_.GetAddressOf() : csMergeList_.GetAddressOf())),
                  ("compute pipeline (" + std::string(entry) + ")").c_str());
            fprintf(stderr, "[d3d12] compute '%s': DXC in %.1fs (%zu bytes)\n", entry,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), code.size());
        }
    }
    for (auto& e : entries) {
        StageLayout dummy;
        auto t0 = std::chrono::steady_clock::now();
        std::vector<uint8_t> code = dxcCompile(dxc, *e.source, e.entry, "cs_" + opt.dxcSm, opt.dxcOpt, dummy);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = !strcmp(e.entry, "tick") ? csTickRoot_.Get() : csRoot_.Get();
        pd.CS = {code.data(), code.size()};
        check(dx_.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(e.pso->GetAddressOf())), ("compute pipeline (" + std::string(e.entry) + ")").c_str());
        fprintf(stderr, "[d3d12] compute '%s': DXC in %.1fs (%zu bytes)\n", e.entry,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), code.size());
        if (!strcmp(e.entry, "tick"))
            fprintf(stderr, "[harness] the processor as a compute shader: %s (%zu bytes)\n", path.c_str(), code.size());
    }
    csStep_ = dx_.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 14;   // the tick's 5 UAV + sync.hlsl's 6 (u2 is Dec in one, the texture in the other) + 3 SRV
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    for (UINT s = 0; s < kSlots; ++s) check(dx_.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&csHeap_[s])), "compute heap");
    return true;
}

// A DEFAULT-heap buffer filled from host memory (init only), left in `after`.
ComPtr<ID3D12Resource> uploadComputeBuffer(Dx& dx, UINT64 size, D3D12_RESOURCE_FLAGS flags, const void* data,
                                           D3D12_RESOURCE_STATES after = D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    ComPtr<ID3D12Resource> dst, up;
    auto bd = bufferDesc(size);
    bd.Flags = flags;
    auto hp = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    check(dx.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&dst)), "compute buffer");
    auto ud = bufferDesc(size);
    auto hu = heapProps(D3D12_HEAP_TYPE_UPLOAD);
    check(dx.dev->CreateCommittedResource(&hu, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&up)), "compute upload");
    uint8_t* p = nullptr;
    check(up->Map(0, nullptr, (void**)&p), "Map compute upload");
    memcpy(p, data, (size_t)size);
    up->Unmap(0, nullptr);
    OneShot os(dx);
    os.list->CopyBufferRegion(dst.Get(), 0, up.Get(), 0, size);
    auto b = transitionBuf(dst.Get(), D3D12_RESOURCE_STATE_COPY_DEST, after);
    os.list->ResourceBarrier(1, &b);
    os.run();
    return dst;
}

void Backend12::setSync12(uint32_t mask, uint32_t mode, uint32_t from, uint32_t count) {
    syncCb_[0] = mask, syncCb_[1] = mode, syncCb_[2] = from, syncCb_[3] = count;
    syncCb_[4] = csMerge_ ? 24u : 32u;
    cl_->SetComputeRoot32BitConstants(2, 8, syncCb_, 0);
}

void Backend12::setRadiate12(uint32_t flips, uint32_t r0s, uint32_t r0e, uint32_t r1s, uint32_t r1e,
                             uint32_t ps, uint32_t pe, uint32_t paint, uint32_t paintVal) {
    radiateSeed_ = radiateSeed_ * 1664525u + 1013904223u;
    radCb_[0] = radiateSeed_, radCb_[1] = flips;
    radCb_[2] = r0s, radCb_[3] = r0e, radCb_[4] = r1s, radCb_[5] = r1e;
    radCb_[6] = ps, radCb_[7] = pe, radCb_[8] = paint, radCb_[9] = paintVal;
    radCb_[10] = 0, radCb_[11] = 0;
    cl_->SetComputeRoot32BitConstants(2, 12, radCb_, 0);
}

void Backend12::uavBarriers(UINT n, ID3D12Resource* const* resources) {
    D3D12_RESOURCE_BARRIER b[8];
    for (UINT i = 0; i < n; ++i) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b[i].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b[i].UAV.pResource = resources[i];
    }
    cl_->ResourceBarrier(n, b);
}

bool Backend12::computeStart(const std::string& imageDir, std::string& err) {
    if (!csCpu_) { err = "no compute processor was built"; return false; }
    std::string payload, rom;
    if (!readFileBinary((fs::u8path(imageDir) / "linux_payload.bin").u8string(), payload) ||
        !readFileBinary((fs::u8path(imageDir) / "rootfs.bin").u8string(), rom)) {
        err = "no image in " + imageDir + " (linux_payload.bin, rootfs.bin)";
        return false;
    }
    {
        // three images of RAM (rv32_min_fast.hlsl): core 0's, the workers', as the last pass left them
        std::vector<uint8_t> ram((size_t)kRamBytes * 3, 0);
        for (int i = 0; i < 3; i++) memcpy(ram.data() + (size_t)kRamBytes * i, payload.data(), (std::min)(payload.size(), (size_t)kRamBytes));
        cpuRam_ = uploadComputeBuffer(dx_, (UINT64)kRamBytes * 3, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, ram.data());
    }
    rom.resize((rom.size() + 15) & ~(size_t)15, 0);
    // the hart as the kernel without an MMU starts: supervisor mode at 0x80400000, the device tree in
    // x11, no reservation; every band of RAM still to go into the texture
    std::vector<uint32_t> state((size_t)computeCores * kCpuStateBytes / 4, 0);
    state[11] = 0x82200000u;
    state[80] = 0x80400000u;
    state[81] = 1;
    state[83] = ~0u;
    state[90] = ~0u;
    const UINT stateBytes = computeCores * kCpuStateBytes;
    cpuState_ = uploadComputeBuffer(dx_, stateBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, state.data());
    cpuRom_ = uploadComputeBuffer(dx_, (UINT64)rom.size(), D3D12_RESOURCE_FLAG_NONE, rom.data(),
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    {
        // decoded instructions, 8 bytes a word of RAM, none made yet; no page or band marked
        std::vector<uint8_t> zeros((size_t)kRamBytes * 2, 0);
        cpuDec_ = uploadComputeBuffer(dx_, (UINT64)kRamBytes * 2, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, zeros.data());
        cpuMarks_ = uploadComputeBuffer(dx_, kMarkBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, zeros.data());
        cpuArgs_ = uploadComputeBuffer(dx_, kArgsBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, zeros.data());
    }
    {
        auto bd = bufferDesc(kCpuStateBytes);
        auto hr = heapProps(D3D12_HEAP_TYPE_READBACK);
        check(dx_.dev->CreateCommittedResource(&hr, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&cpuStateRead_)), "compute state readback");
    }
    // the views that never change: the buffers and the ROM, in every slot's heap
    for (UINT s = 0; s < kSlots; ++s) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = csHeap_[s]->GetCPUDescriptorHandleForHeapStart();
        auto rawUav = [&](ID3D12Resource* buf, UINT64 bytes, UINT k) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
            d.Format = DXGI_FORMAT_R32_TYPELESS;
            d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            d.Buffer.FirstElement = 0;
            d.Buffer.NumElements = (UINT)(bytes / 4);
            d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            D3D12_CPU_DESCRIPTOR_HANDLE at = h;
            at.ptr += (SIZE_T)k * csStep_;
            dx_.dev->CreateUnorderedAccessView(buf, nullptr, &d, at);
        };
        rawUav(cpuRam_.Get(), (UINT64)kRamBytes * 3, 0);   // the tick's table
        rawUav(cpuState_.Get(), stateBytes, 1);
        rawUav(cpuDec_.Get(), (UINT64)kRamBytes * 2, 2);
        rawUav(cpuMarks_.Get(), kMarkBytes, 3);
        rawUav(cpuArgs_.Get(), kArgsBytes, 4);
        rawUav(cpuRam_.Get(), (UINT64)kRamBytes * 3, 5);   // sync.hlsl's table (slot 7: the export's texture, per frame)
        rawUav(cpuState_.Get(), stateBytes, 6);
        rawUav(cpuDec_.Get(), (UINT64)kRamBytes * 2, 8);
        rawUav(cpuMarks_.Get(), kMarkBytes, 9);
        rawUav(cpuArgs_.Get(), kArgsBytes, 10);
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.FirstElement = 0;
        d.Buffer.NumElements = (UINT)(rom.size() / 4);
        d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        D3D12_CPU_DESCRIPTOR_HANDLE at = h;
        at.ptr += (SIZE_T)11 * csStep_;
        dx_.dev->CreateShaderResourceView(cpuRom_.Get(), &d, at);
        at.ptr += (SIZE_T)2 * csStep_;   // the spare SRV: the ROM again, always valid
        dx_.dev->CreateShaderResourceView(cpuRom_.Get(), &d, at);
    }
    fprintf(stderr, "[harness] --compute: %s (%zu bytes) in RAM, rootfs.bin (%zu bytes) as the ROM, %u cores\n", imageDir.c_str(),
            payload.size(), rom.size(), computeCores);
    computeMode = true;
    cpuMode = true;
    memoryViewRadiationImage(0x80000000u, (uint32_t)(payload.size() > 0xffffffffu ? 0xffffffffu : payload.size()));
    return true;
}

// The processor's passes of a frame (computePasses), each a thread group a core and then the
// merge of what core 0 and the workers stored (rv32_min_fast.hlsl); then what was stored into
// the state texture the commit reads, which is state_[cur] here (rvc_backend11's current).
void Backend12::runCompute12(Material& mat, UINT slot, int cur, int dst) {
    const UINT64 ramBytes = kRamBytes;
    UINT64 mtime = (UINT64)mat.getFloat("_HostMtimeLo") | (UINT64)mat.getFloat("_HostMtimeHi") << 32;
    // the export's texture: this frame's current state (sync.hlsl's Out is heap slot 7)
    D3D12_CPU_DESCRIPTOR_HANDLE h = csHeap_[slot]->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE out = h;
    out.ptr += (SIZE_T)7 * csStep_;
    dx_.dev->CreateUnorderedAccessView(state_[cur].Get(), nullptr, nullptr, out);
    ID3D12DescriptorHeap* heaps[] = {csHeap_[slot].Get()};
    cl_->SetDescriptorHeaps(1, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = csHeap_[slot]->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE tickUav = gpu, srvRom = gpu, syncUav = gpu;
    syncUav.ptr += (UINT64)5 * csStep_;
    srvRom.ptr += (UINT64)11 * csStep_;
    cl_->SetComputeRootSignature(csTickRoot_.Get());
    cl_->SetComputeRootDescriptorTable(0, tickUav);   // the tick's u0-u4
    cl_->SetComputeRootDescriptorTable(1, srvRom);   // t0 is the ROM
    ID3D12Resource* buffers[5] = {cpuRam_.Get(), cpuState_.Get(), cpuDec_.Get(), cpuMarks_.Get(), cpuArgs_.Get()};
    // (what the last frame's import wrote: a barrier in this list, as D3D11 orders frames itself)
    uavBarriers(5, buffers);
    // Radiation (fault injection) and paint, from the Radiation panel: flips/paint RAM before
    // the tick runs it. Off by default (no dispatch, no cost).
    radiatePaint_ = ~0u;
    radiatePaintVal_ = 0;
    radiateFlips_ = 0;
    if (csRadiate_) {
        MemoryViewRadiation rad{};
        if (memoryViewRadiation(rad) && ramBytes <= 0xffffffffu) {
            auto clampOff = [&](uint32_t a) { return a < 0x80000000u ? 0u : (a - 0x80000000u > ramBytes ? (uint32_t)ramBytes : a - 0x80000000u); };
            uint32_t r0s = clampOff(rad.r0s) & ~3u, r0e = clampOff(rad.r0e) & ~3u;
            uint32_t r1s = clampOff(rad.r1s) & ~3u, r1e = clampOff(rad.r1e) & ~3u;
            if (r0s >= r0e) r0s = r0e = 0;
            if (r1s >= r1e) r1s = r1e = 0;
            uint32_t ps = 0, pe = 0;
            if (rad.protectImage && rad.imageSize > 0) { ps = 0; pe = rad.imageSize < ramBytes ? rad.imageSize : (uint32_t)ramBytes; }
            uint32_t paint = ~0u;
            if (rad.paint != ~0u) {
                uint32_t p = clampOff(rad.paint) & ~15u;
                if (p + 16 <= ramBytes) paint = p;
            }
            cl_->SetComputeRootSignature(csRoot_.Get());
            cl_->SetComputeRootDescriptorTable(0, syncUav);   // radiate's u0 is Ram
            cl_->SetComputeRootDescriptorTable(1, srvRom);   // (bound, unused)
            setRadiate12((uint32_t)rad.flips, r0s, r0e, r1s, r1e, ps, pe, paint, rad.paintVal);
            cl_->SetPipelineState(csRadiate_.Get());
            cl_->Dispatch(1, 1, 1);
            uavBarriers(5, buffers);
            if (rad.flips > 0) memoryViewRadiationDone((uint64_t)rad.flips);
            radiatePaint_ = paint;   // paint_tex makes it show after the export
            radiatePaintVal_ = rad.paintVal;
            radiateFlips_ = rad.flips;
        }
    }
    const uint32_t split = computeSplit;
    const uint32_t ticks = (uint32_t)mat.getFloat("_Ticks", 8192), slice = split ? (ticks + split - 1) / split : 0;
    for (uint32_t p = 0; p < computePasses; p++) {
        // (every pass its own time, a tick of the clock apart: the kernel tells passes apart by it)
        cpuSeed_ = cpuSeed_ * 1664525u + 1013904223u;
        uint64_t t = mtime + p;
        uint32_t cb[12] = {(uint32_t)t, (uint32_t)(t >> 32), (uint32_t)(int64_t)mat.getFloat("_RTC0"), (uint32_t)(int64_t)mat.getFloat("_RTC1"),
                           ticks, ramBytes, cpuSeed_, 0,   // (0: workers are workers)
                           p, computePasses, computeCores, slice};
        cl_->SetComputeRootSignature(csTickRoot_.Get());
        cl_->SetComputeRootDescriptorTable(0, tickUav);
        cl_->SetComputeRootDescriptorTable(1, srvRom);
        cl_->SetComputeRoot32BitConstants(2, 12, cb, 0);
        cl_->SetPipelineState(csCpu_.Get());
        if (p == 0 || !csMerge_ || p < split) cl_->Dispatch(computeCores, 1, 1);
        else {
            // a pass after the first only when core 0 ended the last early (merge_list says so)
            auto toIa = transitionBuf(cpuArgs_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            cl_->ResourceBarrier(1, &toIa);
            cl_->ExecuteIndirect(csIndirect_.Get(), 1, cpuArgs_.Get(), 24, nullptr, 0);
            auto toUav = transitionBuf(cpuArgs_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl_->ResourceBarrier(1, &toUav);
        }
        // (the tick's stores, the workers' above all: merge_list/count must see them;
        // D3D11 orders dispatches itself, D3D12 needs the barrier said out loud)
        uavBarriers(5, buffers);
        if (csMerge_) {
            // the pages stored to, listed; then a thread group each, as many as listed
            cl_->SetPipelineState(csMergeList_.Get());
            cl_->Dispatch(1, 1, 1);
            uavBarriers(5, buffers);
            auto toIa = transitionBuf(cpuArgs_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            cl_->ResourceBarrier(1, &toIa);
            cl_->SetPipelineState(csMerge_.Get());
            cl_->ExecuteIndirect(csIndirect_.Get(), 1, cpuArgs_.Get(), 0, nullptr, 0);
            auto toUav = transitionBuf(cpuArgs_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl_->ResourceBarrier(1, &toUav);
            uavBarriers(5, buffers);
        }
    }
    // Linux's own bands (which only the memory view shows) go into the texture at first and every
    // 60th frame while it is open; the GPU's memory and the control words every frame, as --cpu.
    // Painted/flipped texels go at once (their bands are marked, the mask stays whole while radiating).
    uint32_t mask = computeMask;
    bool fullBands = computeFrames_++ == 0 || (viewOpen() && computeFrames_ % 60 == 0);
    if (radiatePaint_ == ~0u && radiateFlips_ == 0 && csMerge_ && !fullBands) mask &= 0xff000000u;
    cl_->SetComputeRootSignature(csRoot_.Get());
    cl_->SetComputeRootDescriptorTable(0, syncUav);   // sync.hlsl's u0-u5
    cl_->SetComputeRootDescriptorTable(1, srvRom);
    setSync12(mask, 0, 0, computeCores);
    cl_->SetPipelineState(csCount_.Get());
    cl_->Dispatch(1, 1, 1);
    uavBarriers(5, buffers);
    // (the export writes the texture the commit reads: as UAV now, the commit's SRV after)
    auto toUav = transition(state_[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl_->ResourceBarrier(1, &toUav);
    cl_->SetPipelineState(csState_.Get());
    cl_->Dispatch(1, 16, 1);
    if (csMerge_) {
        // the GPU's pages stored to in this frame (merge_list listed them), a group each;
        // Linux's bands whole when they go
        cl_->SetPipelineState(csExportPages_.Get());
        auto toIa = transitionBuf(cpuArgs_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        cl_->ResourceBarrier(1, &toIa);
        cl_->ExecuteIndirect(csIndirect_.Get(), 1, cpuArgs_.Get(), 12, nullptr, 0);
        auto toUavArgs = transitionBuf(cpuArgs_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl_->ResourceBarrier(1, &toUavArgs);
    }
    if (!csMerge_ || (mask & 0x00ffffffu) != 0) {
        // (after export_pages when it ran: both write the texture, different words, but said out loud)
        if (csMerge_) { ID3D12Resource* et = state_[cur].Get(); uavBarriers(1, &et); }
        cl_->SetPipelineState(csExport_.Get());
        cl_->Dispatch(8, kHeight - 64, 1);
    }
    if (radiatePaint_ != ~0u && csPaintTex_) {
        // the cursor's paint into the fresh texture: clean pages are not exported,
        // so RAM alone would not show it. (RAM already has it, for the tick.)
        // (The committed buffer is left alone: transitioning it broke the workers.)
        cl_->SetComputeRootSignature(csRoot_.Get());
        cl_->SetComputeRootDescriptorTable(0, syncUav);
        cl_->SetComputeRootDescriptorTable(1, srvRom);   // (bound, unused)
        setRadiate12(0, 0, 0, 0, 0, 0, 0, radiatePaint_, radiatePaintVal_);
        cl_->SetPipelineState(csPaintTex_.Get());
        ID3D12Resource* pt = state_[cur].Get();
        uavBarriers(1, &pt);
        cl_->Dispatch(1, 1, 1);   // state_[cur] (slot 7 already views it)
    }
    ID3D12Resource* curTex = state_[cur].Get();
    uavBarriers(1, &curTex);
    auto toSrv = transition(state_[cur].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cl_->ResourceBarrier(1, &toSrv);
}

// What the devices wrote into RAM in this frame, back into the processor's (rvc_backend11).
// The committed texture is state_[dst]; it is left for the readback's COPY_SOURCE transition.
void Backend12::computeImport12(bool con, bool fetch, bool net, bool counts, UINT slot, int dst) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = csHeap_[slot]->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)12 * csStep_;
    dx_.dev->CreateShaderResourceView(state_[dst].Get(), nullptr, h);
    ID3D12DescriptorHeap* heaps[] = {csHeap_[slot].Get()};
    cl_->SetDescriptorHeaps(1, heaps);
    cl_->SetComputeRootSignature(csRoot_.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = csHeap_[slot]->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE sync = gpu;
    sync.ptr += (UINT64)5 * csStep_;
    cl_->SetComputeRootDescriptorTable(0, sync);   // sync.hlsl's u0-u5
    D3D12_GPU_DESCRIPTOR_HANDLE stable = gpu;
    stable.ptr += (UINT64)12 * csStep_;   // t0 is the committed texture now
    cl_->SetComputeRootDescriptorTable(1, stable);
    auto toSrv = transition(state_[dst].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl_->ResourceBarrier(1, &toSrv);
    ID3D12Resource* buffers[5] = {cpuRam_.Get(), cpuState_.Get(), cpuDec_.Get(), cpuMarks_.Get(), cpuArgs_.Get()};
    uavBarriers(5, buffers);
    cl_->SetPipelineState(csPlan_.Get());
    cl_->Dispatch(1, 1, 1);
    uavBarriers(5, buffers);
    cl_->SetPipelineState(csTake_.Get());
    setSync12(0, 1, 0, 0);   // (first: its words are the control words before they come back)
    cl_->Dispatch((kGpuTarget * kGpuTarget / 4 + 1 + 255) / 256, 1, 1);
    uavBarriers(5, buffers);
    auto texels = [&](uint32_t from, uint32_t count) {
        setSync12(0, 0, from, count);
        cl_->Dispatch((count + 255) / 256, 1, 1);
    };
    const uint32_t net0 = (kNetRow - 64) * 2048;
    texels((kControlRow - 64) * 2048, kControlTexels);
    if (con) texels(net0 + kConHost, 1);
    if (net) texels(net0 + kNetRing, kNetSlot * kNetSlots);
    if (fetch) texels(0x76c000, 0x4000);   // FETCH_DATA (src/gpu.h); its words are control words
    if (counts) texels(0x6c1000, 256);    // COUNT_TOTAL: the counters' totals the control pass added up
    uavBarriers(5, buffers);
    // the cores as the control words have them (after those came back)
    setSync12(0, 0, 0, computeCores);
    cl_->SetPipelineState(csCores_.Get());
    cl_->Dispatch(1, 1, 1);
    uavBarriers(5, buffers);
}

bool Backend12::computeState(std::vector<uint32_t>& out) {
    if (!cpuState_) return false;
    OneShot os(dx_);
    auto b0 = transitionBuf(cpuState_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    os.list->ResourceBarrier(1, &b0);
    os.list->CopyBufferRegion(cpuStateRead_.Get(), 0, cpuState_.Get(), 0, kCpuStateBytes);
    auto b1 = transitionBuf(cpuState_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    os.list->ResourceBarrier(1, &b1);
    os.run();
    uint8_t* p = nullptr;
    if (FAILED(cpuStateRead_->Map(0, nullptr, (void**)&p))) return false;
    out.assign((const uint32_t*)p, (const uint32_t*)p + kCpuStateBytes / 4);
    cpuStateRead_->Unmap(0, nullptr);
    return true;
}

}  // namespace

std::unique_ptr<RvcBackend> makeBackend12() { return std::make_unique<Backend12>(); }
