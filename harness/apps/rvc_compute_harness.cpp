// The machine's processor as one compute shader thread (experiments/rv32_min): an image's RAM
// and ROM, passes of the shader, the console in and out, instructions counted. No GPU, sound
// or network device, no worker cores: Linux to its shell and what runs on the console.
#include "run_lock.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <fcntl.h>
#include <io.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

static const uint32_t RAM_BASE = 0x80000000u, RAM_SIZE = 0x12000000u;   // as rvc_cpu's
static const uint32_t CON = 0x876b8280u;   // docs/machine/console.md: host words, guest words, the ring
static const uint32_t CON_BYTES = 0x120u, RING = 0x20u, RING_SIZE = 256;
static const uint32_t STATE_WORDS = 90, S_PC = 80, S_PRIV = 81, S_RES = 83, S_TCMP = 84, S_RAN = 89;

struct Options {
    std::string shader = "experiments/rv32_min/rv32_min_fast.hlsl";
    std::string image = "build/images/nommu";
    uint32_t ticks = 8192;
    uint64_t frames = 0;
    double seconds = 0, fixedDt = 0;
    std::string input, until, statsFrom;
    std::vector<std::pair<std::string, std::string>> expectSend;
    std::vector<std::pair<std::string, std::string>> defines;
    bool quiet = false;
};

static std::string unescape(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { r += s[i]; continue; }
        char c = s[++i];
        if (c == 'n') r += '\n';
        else if (c == 'r') r += '\r';
        else if (c == 't') r += '\t';
        else if (c == 'x' && i + 2 < s.size()) { r += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else { r += '\\'; r += c; }
    }
    return r;
}

static void usage() {
    fprintf(stderr,
            "rvc_compute_harness: the no-MMU machine's processor as one compute shader thread\n"
            "  --shader FILE        the shader (default experiments/rv32_min/rv32_min_fast.hlsl)\n"
            "  --image DIR|linux-net  linux_payload.bin and rootfs.bin (default build/images/nommu)\n"
            "  --ticks N            instructions a pass at most (default 8192)\n"
            "  --input TEXT         typed at the console from the start (\\n and the like)\n"
            "  --expect TEXT --send TEXT  typed once the console has said TEXT, in order\n"
            "  --until TEXT         stop once the console says TEXT (exit 3 if a limit comes first)\n"
            "  --stats-from TEXT    print a STATS line for the run from where the console says TEXT\n"
            "  --seconds S, --frames N  stop after S seconds of wall time or N passes\n"
            "  --fixed-dt S         guest time S a pass, and on to the timer when idle (default: wall time)\n"
            "  --define NAME[=V]    a define for the shader\n"
            "  --quiet              no console output\n");
}

static bool parse(int argc, char** argv, Options& o) {
    std::string expect;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); exit(1); }
            return argv[++i];
        };
        if (a == "--shader") o.shader = next();
        else if (a == "--image") { std::string v = next(); o.image = v == "linux-net" ? "build/images/nommu" : v; }
        else if (a == "--ticks") o.ticks = (uint32_t)strtoul(next().c_str(), nullptr, 10);
        else if (a == "--frames") o.frames = strtoull(next().c_str(), nullptr, 10);
        else if (a == "--seconds") o.seconds = atof(next().c_str());
        else if (a == "--fixed-dt") o.fixedDt = atof(next().c_str());
        else if (a == "--input") o.input += unescape(next());
        else if (a == "--expect") expect = unescape(next());
        else if (a == "--send") { o.expectSend.push_back({expect, unescape(next())}); expect.clear(); }
        else if (a == "--until") o.until = unescape(next());
        else if (a == "--stats-from") o.statsFrom = unescape(next());
        else if (a == "--define") {
            std::string d = next();
            size_t eq = d.find('=');
            o.defines.push_back({d.substr(0, eq), eq == std::string::npos ? "1" : d.substr(eq + 1)});
        }
        else if (a == "--quiet") o.quiet = true;
        else if (a == "--help" || a == "-h") { usage(); exit(0); }
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return false; }
    }
    return true;
}

static bool read_file(const std::string& path, std::vector<uint8_t>& to) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    to.resize((size_t)n);
    size_t got = fread(to.data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

static ComPtr<ID3D11Buffer> make_buffer(ID3D11Device* dev, uint32_t bytes, UINT bind, UINT misc, D3D11_USAGE usage,
                                        UINT cpu, const void* init) {
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = bytes;
    d.Usage = usage;
    d.BindFlags = bind;
    d.MiscFlags = misc;
    d.CPUAccessFlags = cpu;
    D3D11_SUBRESOURCE_DATA s = {init, 0, 0};
    ComPtr<ID3D11Buffer> b;
    HRESULT hr = dev->CreateBuffer(&d, init ? &s : nullptr, &b);
    if (FAILED(hr)) { fprintf(stderr, "CreateBuffer of %u bytes failed: %08lx\n", bytes, (unsigned long)hr); exit(1); }
    return b;
}

static ComPtr<ID3D11UnorderedAccessView> raw_uav(ID3D11Device* dev, ID3D11Buffer* b, uint32_t bytes) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = bytes / 4;
    d.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    ComPtr<ID3D11UnorderedAccessView> v;
    if (FAILED(dev->CreateUnorderedAccessView(b, &d, &v))) { fprintf(stderr, "a UAV failed\n"); exit(1); }
    return v;
}

// The clock chip's two words, as rvc_harness gives them to the shader machine.
static void rtc_words(uint32_t& rtc0, uint32_t& rtc1) {
    auto bcd = [](int v) { return (uint32_t)((v / 10) << 4 | (v % 10)); };
    time_t clock = time(nullptr);
    struct tm lt{};
    localtime_s(&lt, &clock);
    int year = lt.tm_year + 1900;
    rtc0 = bcd(year / 100) | bcd(lt.tm_sec) << 8 | bcd(lt.tm_min) << 16 | bcd(lt.tm_hour) << 24;
    rtc1 = bcd(lt.tm_wday + 1) | bcd(lt.tm_mday) << 8 | bcd(lt.tm_mon + 1) << 16 | bcd(year % 100) << 24;
}

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) return 1;
    if (!run_lock_take(true, "rvc_compute_harness")) return kRunLockRefused;
    _setmode(_fileno(stdout), _O_BINARY);   // the guest's line ends as they are

    std::vector<uint8_t> payload, rom, source;
    if (!read_file(opt.image + "/linux_payload.bin", payload) || !read_file(opt.image + "/rootfs.bin", rom)) {
        fprintf(stderr, "no image in %s (linux_payload.bin, rootfs.bin)\n", opt.image.c_str());
        return 1;
    }
    if (!read_file(opt.shader, source)) { fprintf(stderr, "no shader %s\n", opt.shader.c_str()); return 1; }

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &want, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        fprintf(stderr, "no D3D11 device\n");
        return 1;
    }
    {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC ad = {};
        if (SUCCEEDED(dev.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&ad)))
            fprintf(stderr, "[compute] %ls\n", ad.Description);
    }

    std::vector<D3D_SHADER_MACRO> macros;
    for (auto& d : opt.defines) macros.push_back({d.first.c_str(), d.second.c_str()});
    macros.push_back({nullptr, nullptr});
    ComPtr<ID3DBlob> code, errors;
    auto c0 = std::chrono::steady_clock::now();
    HRESULT hr = D3DCompile(source.data(), source.size(), opt.shader.c_str(), macros.data(), nullptr, "tick", "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    double compile_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    if (errors && errors->GetBufferSize()) fprintf(stderr, "%s", (const char*)errors->GetBufferPointer());
    if (FAILED(hr)) { fprintf(stderr, "the shader did not compile\n"); return 1; }
    fprintf(stderr, "[compute] %s compiled in %.2f s, %zu bytes\n", opt.shader.c_str(), compile_s, (size_t)code->GetBufferSize());
    ComPtr<ID3D11ComputeShader> cs;
    if (FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs))) return 1;

    std::vector<uint8_t> ram(RAM_SIZE, 0);
    memcpy(ram.data(), payload.data(), payload.size() < RAM_SIZE ? payload.size() : RAM_SIZE);
    rom.resize((rom.size() + 15) & ~(size_t)15, 0);
    uint32_t state0[STATE_WORDS] = {};
    state0[11] = 0x82200000u;   // the device tree, for the kernel in supervisor mode at 0x80400000
    state0[S_PC] = 0x80400000u;
    state0[S_PRIV] = 1;
    state0[S_RES] = ~0u;

    const UINT RAW = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    auto ramBuf = make_buffer(dev.Get(), RAM_SIZE, D3D11_BIND_UNORDERED_ACCESS, RAW, D3D11_USAGE_DEFAULT, 0, ram.data());
    auto stateBuf = make_buffer(dev.Get(), STATE_WORDS * 4, D3D11_BIND_UNORDERED_ACCESS, RAW, D3D11_USAGE_DEFAULT, 0, state0);
    auto romBuf = make_buffer(dev.Get(), (uint32_t)rom.size(), D3D11_BIND_SHADER_RESOURCE, RAW, D3D11_USAGE_DEFAULT, 0, rom.data());
    auto cbuf = make_buffer(dev.Get(), 32, D3D11_BIND_CONSTANT_BUFFER, 0, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, nullptr);
    auto stateRead = make_buffer(dev.Get(), STATE_WORDS * 4, 0, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, nullptr);
    auto conRead = make_buffer(dev.Get(), CON_BYTES, 0, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, nullptr);
    ram.clear();
    ram.shrink_to_fit();
    auto ramUav = raw_uav(dev.Get(), ramBuf.Get(), RAM_SIZE);
    auto stateUav = raw_uav(dev.Get(), stateBuf.Get(), STATE_WORDS * 4);
    ComPtr<ID3D11ShaderResourceView> romSrv;
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        d.BufferEx.NumElements = (UINT)(rom.size() / 4);
        d.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (FAILED(dev->CreateShaderResourceView(romBuf.Get(), &d, &romSrv))) { fprintf(stderr, "the ROM's view failed\n"); return 1; }
    }
    rom.clear();
    rom.shrink_to_fit();

    ctx->CSSetShader(cs.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* uavs[2] = {ramUav.Get(), stateUav.Get()};
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ID3D11ShaderResourceView* srvs[1] = {romSrv.Get()};
    ctx->CSSetShaderResources(0, 1, srvs);
    ID3D11Buffer* cbs[1] = {cbuf.Get()};
    ctx->CSSetConstantBuffers(0, 1, cbs);

    std::deque<char> typed(opt.input.begin(), opt.input.end());
    size_t nextExpect = 0, expectFrom = 0;
    std::string transcript;
    uint32_t read = 0, rtc0 = 0, rtc1 = 0, seed = 0x9e3779b9u;
    bool seen = false, untilHit = false, statsOn = opt.statsFrom.empty();
    uint64_t frame = 0, instructions = 0, idle = 0, statsInstr = 0, statsFrame = 0;
    uint32_t con[CON_BYTES / 4] = {}, lastPc = 0;
    double guest = 0, statsT0 = 0, last_rtc = -1;
    auto t0 = std::chrono::steady_clock::now();
    auto wall = [&]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    int exitCode = 0;

    for (;; frame++) {
        double now = wall();
        if (opt.fixedDt <= 0) guest = now;
        if (now - last_rtc >= 0.5) { rtc_words(rtc0, rtc1); last_rtc = now; }
        uint64_t mtime = (uint64_t)(guest * 5000.0);   // 5,000 a second, as the shader machine's
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(cbuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            seed = seed * 1664525u + 1013904223u;
            uint32_t cb[8] = {(uint32_t)mtime, (uint32_t)(mtime >> 32), rtc0, rtc1, opt.ticks, RAM_SIZE, seed, 0};
            memcpy(m.pData, cb, sizeof cb);
            ctx->Unmap(cbuf.Get(), 0);
        }
        // up to four characters, once the guest has taken everything delivered before
        if (con[5] != 0 && con[4] == con[0] && !typed.empty()) {
            uint32_t group = 0, n = 0;
            for (; n < 4 && !typed.empty(); n++, typed.pop_front()) group |= (uint32_t)(uint8_t)typed.front() << (8 * n);
            uint32_t words[2] = {con[0] + n, group};
            D3D11_BOX box = {CON - RAM_BASE, 0, 0, CON - RAM_BASE + 8, 1, 1};
            ctx->UpdateSubresource(ramBuf.Get(), 0, &box, words, 0, 0);
        }
        ctx->Dispatch(1, 1, 1);
        ctx->CopyResource(stateRead.Get(), stateBuf.Get());
        D3D11_BOX conBox = {CON - RAM_BASE, 0, 0, CON - RAM_BASE + CON_BYTES, 1, 1};
        ctx->CopySubresourceRegion(conRead.Get(), 0, 0, 0, 0, ramBuf.Get(), 0, &conBox);
        uint32_t st[STATE_WORDS];
        if (FAILED(ctx->Map(stateRead.Get(), 0, D3D11_MAP_READ, 0, &m))) { fprintf(stderr, "readback failed (device removed?)\n"); return 1; }
        memcpy(st, m.pData, sizeof st);
        ctx->Unmap(stateRead.Get(), 0);
        if (FAILED(ctx->Map(conRead.Get(), 0, D3D11_MAP_READ, 0, &m))) return 1;
        memcpy(con, m.pData, sizeof con);
        ctx->Unmap(conRead.Get(), 0);

        uint32_t ran = st[S_RAN];
        lastPc = st[S_PC];
        instructions += ran;
        // the ring: bytes the guest has written so far in con[5], byte n at n mod 256
        uint32_t written = con[5];
        if (!seen) read = written, seen = true;
        if (written - read > RING_SIZE) read = written - RING_SIZE;
        size_t before = transcript.size();
        for (; read != written; read++) {
            char c = (char)((const uint8_t*)con)[RING + (read & (RING_SIZE - 1))];
            if (!c) continue;
            transcript += c;
            if (!opt.quiet) fputc(c, stdout);
        }
        if (transcript.size() != before) {
            if (!opt.quiet) fflush(stdout);
            size_t back = 64;
            size_t from = before > back ? before - back : 0;
            while (nextExpect < opt.expectSend.size()) {
                auto& es = opt.expectSend[nextExpect];
                size_t at = es.first.empty() ? expectFrom : transcript.find(es.first, expectFrom);
                if (at == std::string::npos) break;
                expectFrom = at + es.first.size();
                typed.insert(typed.end(), es.second.begin(), es.second.end());
                nextExpect++;
            }
            if (!statsOn && transcript.find(opt.statsFrom, from) != std::string::npos) {
                statsOn = true;
                statsT0 = wall();
                statsInstr = instructions;
                statsFrame = frame;
            }
            if (!opt.until.empty() && transcript.find(opt.until, from) != std::string::npos) { untilHit = true; break; }
        }
        if (ran < opt.ticks) {
            idle++;
            // nothing to run until the timer: in fixed time go on to it, in wall time give the processor back
            uint64_t tcmp = (uint64_t)st[S_TCMP + 1] << 32 | st[S_TCMP];
            if (opt.fixedDt > 0 && tcmp > mtime && typed.empty()) {
                double until = (double)tcmp / 5000.0;
                if (until < guest + 1.0) guest = until;
            } else if (opt.fixedDt <= 0 && typed.empty()) {
                Sleep(0);
            }
        }
        if (opt.fixedDt > 0) guest += opt.fixedDt;
        if (opt.frames && frame + 1 >= opt.frames) { exitCode = opt.until.empty() ? 0 : 3; break; }
        if (opt.seconds > 0 && wall() >= opt.seconds) { exitCode = opt.until.empty() ? 0 : 3; break; }
    }
    double secs = wall();
    if (!opt.quiet) fputc('\n', stdout);
    fprintf(stderr, "[compute] %llu passes in %.1f s, %llu instructions, %.0fk instructions/s, %llu passes idle; pc %08x, --until %s\n",
            (unsigned long long)(frame + 1), secs, (unsigned long long)instructions, instructions / secs / 1e3,
            (unsigned long long)idle, lastPc, opt.until.empty() ? "not given" : untilHit ? "matched" : "NOT matched");
    if (!opt.statsFrom.empty()) {
        if (statsOn) {
            double dt = secs - statsT0;
            uint64_t n = instructions - statsInstr, f = frame + 1 - statsFrame;
            fprintf(stderr, "STATS seconds=%.2f instructions=%llu ips=%.0f frames=%llu fps=%.1f per_frame=%.1f\n", dt,
                    (unsigned long long)n, n / dt, (unsigned long long)f, f / dt, f ? (double)n / f : 0.0);
        } else {
            fprintf(stderr, "STATS not started: the console never said the --stats-from text\n");
        }
    }
    return exitCode;
}
