// nxpath: a path tracer of a Cornell box, its tiles traced by every worker core the machine
// has, with its settings in a Dear ImGui panel (docs/raytrace.md, docs/multicore.md,
// docs/imgui.md). Core 0 traces nothing: it draws the panel and the picture as it clears,
// and hands a tile to each core that has none.
//
// For tests: NXPATH_AUTO=1 presses Render at the start, NXPATH_EXIT=1 leaves when the picture
// is done, NXPATH_SAMPLES, NXPATH_BOUNCES, NXPATH_SIZE=WxH, NXPATH_CORES=N (0 to 3: which
// of the panel's layouts of the cores), NXPATH_SCENE=N (0: the balls; from 1, a model) and
// NXPATH_FIRST=N (the first thing's material, 0 to 2) set the panel. NXPATH_PRINT=1 prints the
// picture, and with NXPATH_DEPTH=1 it is of distances (tools/pathtrace_reference.py). Every picture's line (pathstat:) has its
// time, its rays and a sum of its pixels, which is the same whichever cores traced it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "imgui.h"
#include "imgui_impl_shaderemu.h"
extern "C" {
#include <GLES/segl.h>
#include <nano-X.h>
#include "mcw.h"
#include "tracer.h"
extern char __DATA_BEGIN__[], _end[];
float sinf(float), cosf(float);
void sincosf(float, float*, float*);
}

#define MOST_W 640
#define MOST_H 480
#define SIZES 6

static unsigned now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)ts.tv_sec * 1000u + (unsigned)ts.tv_nsec / 1000000u;
}

static void pass()
{
    mcw_rest();   // nothing to do: the kernel has the core until the next pass (the pause hint with no workers)
}

// The panel's circles ask for the same sines and cosines at every frame, and the C library
// works each out in doubles, which are calls here (a third of a frame of the panel). The
// library's own answers are kept by the angle's bits, and ImGui's calls come here.
void sincosf(float x, float* s, float* c)
{
    static struct { unsigned bits; bool kept; float s, c; } known[256];
    union { float f; unsigned u; } angle = {x};
    auto& k = known[(angle.u * 2654435761u) >> 24];
    if (!k.kept || k.bits != angle.u) {
        volatile float again = x;   // (asked apart, or the compiler makes the two one call of this)
        k.s = sinf(x);
        k.c = cosf(again);
        k.bits = angle.u;
        k.kept = true;
    }
    *s = k.s;
    *c = k.c;
}

// A worker stops at a page nobody has read yet: a model's boxes and triangles are read here first.
static void read_model(int scene)
{
    if (scene < 1) return;
    const struct pt_mesh& m = pt_meshes[scene - 1];
    const volatile char* part[2] = {(const char*)m.nodes, (const char*)m.triangles};
    size_t bytes[2] = {(size_t)m.node_count * sizeof(struct pt_node), (size_t)m.triangle_count * sizeof(struct pt_triangle)};
    for (int i = 0; i < 2; i++) {
        for (size_t at = 0; at < bytes[i]; at += 4096) (void)part[i][at];
        (void)part[i][bytes[i] - 1];
    }
}

// How many of the machine's worker cores the panel offers to trace with: all, half, a quarter,
// none. They are this program's share (docs/multicore.md): with another program at work on
// them too it has fewer, which the line under the picture says.
static char layout_names[4][40] = {"", "", "", "this core alone"};
static int layout_workers[4];
static void make_layouts()
{
    static const char* const what[3] = {"every worker core", "half of them", "a quarter"};
    for (int i = 0; i < 3; i++) {
        layout_workers[i] = mcw_most() >> i;
        snprintf(layout_names[i], sizeof layout_names[i], "%s (%d)", what[i], layout_workers[i]);
    }
}
static int workers = 0, layout_set = -1;

// The workers given back, and as many taken as the choice says.
static void take_workers(int layout)
{
    if (layout == layout_set) return;
    mcw_close();
    workers = 0;
    layout_set = layout;
    if (layout >= 3 || layout_workers[layout] <= 0) return;
    workers = mcw_open(layout_workers[layout]);
    if (workers) mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
}

int main()
{
    if (GrOpen() < 0) {
        fprintf(stderr, "nxpath: no Nano-X server (start one: nano-X -p &)\n");
        return 1;
    }
    GR_WINDOW_ID window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, (char*)"Path tracer", GR_ROOT_WINDOW_ID, -1, -1, 780, 520, 0);
    GrMapWindow(window);
    if (seglInit(window) < 0) {
        fprintf(stderr, "nxpath: this machine has no GPU\n");
        return 1;
    }
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplShaderEmu_Init(window)) {
        fprintf(stderr, "nxpath: no GPU memory for the font\n");
        return 1;
    }
    // the picture: a texture where it lies, in GPU memory, and the samples' sums in memory
    // that begins on 16 bytes (a worker's own to write, tile by tile). Neither is cleared or
    // touched here: a picture's own part of both is, when it is asked for.
    pt.pixels = (uint32_t*)seglMemory(MOST_W * MOST_H * 4);
    pt.sums = (float*)mcw_alloc(MOST_W * MOST_H * 3 * sizeof(float));
    if (!pt.pixels || !pt.sums) {
        fprintf(stderr, "nxpath: no memory for the picture\n");
        return 1;
    }
    GLuint texture;
    glGenTextures(1, &texture);
    atexit(mcw_close);

    // the panel's settings
    static const int sizes[SIZES][2] = {{160, 120}, {240, 180}, {320, 240}, {400, 300}, {480, 360}, {640, 480}};
    static const char* const size_names[] = {"160 x 120", "240 x 180", "320 x 240", "400 x 300", "480 x 360", "640 x 480"};
    static const char* const materials[] = {"matt", "mirror", "glass"};
    int size = 0, samples = 16, bounces = 5, layout = 0, ball[2] = {PT_MIRROR, PT_GLASS}, scene = 0;
    // what stands in the box: the two balls and the block, or a model of triangles (tracer.h)
    static const char* scene_names[8] = {"two balls and a block"};
    for (int i = 0; i < pt_mesh_count && i < 7; i++) scene_names[i + 1] = pt_meshes[i].name;
    float light = 12.0f;
    bool owners = false, leave_when_done = getenv("NXPATH_EXIT") != nullptr, press = getenv("NXPATH_AUTO") != nullptr;
    const char* text;
    if ((text = getenv("NXPATH_SAMPLES")) != nullptr) samples = atoi(text);
    if ((text = getenv("NXPATH_BOUNCES")) != nullptr) bounces = atoi(text);
    if ((text = getenv("NXPATH_CORES")) != nullptr) layout = atoi(text) & 3;
    if ((text = getenv("NXPATH_SCENE")) != nullptr && atoi(text) >= 0 && atoi(text) <= pt_mesh_count) scene = atoi(text);
    if ((text = getenv("NXPATH_FIRST")) != nullptr) ball[0] = atoi(text) % 3;
    if ((text = getenv("NXPATH_SIZE")) != nullptr)
        for (int i = 0; i < SIZES; i++)
            if (atoi(text) == sizes[i][0]) size = i;

    // the picture being traced
    bool tracing = false, drawn_once = false;
    int tiles = 0, sample = 0, next = 0, out = 0, pictures = 0, shown_w = 160, shown_h = 120, of_samples = 0, have = 0;
    static uint8_t busy[MC_MAX_CORES];
    unsigned began = 0, took = 0, rays = 0, drew = 0;
    bool stopping = false;
    make_layouts();
    pt.slots = (struct pt_slot*)mcw_alloc(MC_MAX_CORES * sizeof(struct pt_slot));

    for (;;) {
        // ---- the shares: every worker has all of its own at once, and is asked how far it is ----
        bool idle = true;
        if (tracing) {
            if (workers == 0 && next < tiles) {
                // this core alone: a tile, then the panel
                rays += pt_tile((uint32_t)next++ * pt.stride % (uint32_t)tiles, (uint32_t)sample);
                have++;
                idle = false;
            }
            if (workers) {
                unsigned all = 0;
                int least = of_samples, done = 0;
                for (int k = 1; k <= workers; k++) {
                    if (busy[k] && mcw_done(k)) {
                        busy[k] = 0;
                        out--;
                    }
                    int at = (int)pt.slots[k].samples;
                    if (!busy[k] && at < of_samples && !stopping) {
                        // (its core was wanted elsewhere, or it has none and this one works for it: the rest, again)
                        busy[k] = 1;
                        out++;
                        mcw_post(k, pt_share, (uint32_t)(k - 1) | (uint32_t)workers << 16, (uint32_t)k << 24 | (mcw_bound(k) ? 0u : 1u));
                        idle = false;
                    }
                    all += pt.slots[k].rays;
                    done += at;
                    if (at < least) least = at;
                }
                rays = all;
                sample = least;
                have = done;
                if (out == 0 && (least >= of_samples || stopping)) next = tiles;
            }
            if (next >= tiles && out == 0) {
                // a sample of every pixel is in: the next, or the picture is done
                took = now_ms() - began;
                if (workers) {
                    // (given up: as many samples as every share has; the tiles of some have more)
                    if (stopping) of_samples = sample > 0 ? sample : 1;
                    sample = of_samples - 1;
                }
                stopping = false;
                if (++sample >= of_samples) {
                    unsigned sum = 0;
                    for (int i = 0; i < pt.width * pt.height; i++) sum = sum * 31 + pt.pixels[i];
                    tracing = false;
                    fprintf(stderr, "pathstat: picture %d, %d x %d, %d samples, %d turns: %u ms with %d workers (%s), %u rays, %u thousand rays a second, sum %08x\n",
                            pictures++, pt.width, pt.height, of_samples, pt.bounces, took, workers, layout_names[layout_set < 0 ? 3 : layout_set], rays,
                            took ? rays / took : 0, sum);
                    if (getenv("NXPATH_PRINT")) {
                        // (for a test: the picture on the console, a row a line, six hex digits a pixel)
                        for (int y = 0; y < pt.height; y++) {
                            fprintf(stderr, "pathrow %d ", y);
                            for (int x = 0; x < pt.width; x++) fprintf(stderr, "%06x", (unsigned)(pt.pixels[y * pt.width + x] & 0xffffff));
                            fprintf(stderr, "\n");
                        }
                    }
                    if (leave_when_done) break;
                } else {
                    next = 0;
                }
                idle = false;
            }
        }
        // ---- the panel, ten times a second (it is 200 thousand instructions of this core) ----
        unsigned now = now_ms();
        if (drawn_once && now - drew < (tracing ? 150u : 50u)) {
            if (idle) pass();
            continue;
        }
        drew = now;
        drawn_once = true;
        if (!ImGui_ImplShaderEmu_NewFrame()) break;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(270, 500), ImGuiCond_FirstUseEver);
        ImGui::Begin("Path tracer");
        ImGui::TextWrapped("A Cornell box and what stands in it, traced by every worker core of the machine.");
        ImGui::Separator();
        ImGui::BeginDisabled(tracing);
        ImGui::Combo("picture", &size, size_names, SIZES);
        ImGui::SliderInt("samples", &samples, 1, 256, "%d a pixel", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderInt("turns", &bounces, 1, 8, "%d of a path");
        ImGui::SliderFloat("lamp", &light, 2.0f, 30.0f, "%.0f");
        ImGui::Combo("in the box", &scene, scene_names, pt_mesh_count + 1);
        if (scene) ImGui::Text("%d triangles, %d boxes", pt_meshes[scene - 1].triangle_count, pt_meshes[scene - 1].node_count);
        ImGui::Combo(scene ? "the model" : "left ball", &ball[0], materials, 3);
        ImGui::Combo(scene ? "its ball" : "right ball", &ball[1], materials, 3);
        ImGui::Checkbox("show which core traced what", &owners);
        ImGui::Separator();
        ImGui::TextUnformatted("The cores:");
        for (int i = 0; i < 4; i++) ImGui::RadioButton(layout_names[i], &layout, i);
        ImGui::EndDisabled();
        ImGui::Separator();
        bool render = ImGui::Button(tracing ? "Tracing..." : "Render", ImVec2(120, 28)) && !tracing;
        ImGui::SameLine();
        bool stop = ImGui::Button("Stop", ImVec2(60, 28)) && tracing;
        if (tracing || pictures) {
            int all = (workers ? workers : tiles) * (of_samples > 0 ? of_samples : 1);
            ImGui::ProgressBar(all && tracing ? (float)have / (float)all : 1.0f, ImVec2(-1, 0));
            unsigned ms = tracing ? now - began : took;
            ImGui::Text("%d of %d samples, %u.%u s", tracing ? sample : of_samples, of_samples, ms / 1000, ms % 1000 / 100);
            ImGui::Text("%u,%03u rays, %u thousand a second", rays / 1000, rays % 1000, ms ? rays / ms : 0);
            ImGui::Text("%d worker cores", workers);
        }
        bool quit = ImGui::Button("Quit");
        ImGui::End();
        ImGui::SetNextWindowPos(ImVec2(286, 8), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(486, 500), ImGuiCond_FirstUseEver);
        ImGui::Begin("Picture");
        {
            // as large as fits: whole pixels when there is room for more than one, smaller when not for one
            ImVec2 room = ImGui::GetContentRegionAvail();
            float by = room.x / (float)shown_w < room.y / (float)shown_h ? room.x / (float)shown_w : room.y / (float)shown_h;
            if (by >= 1.0f) by = (float)(int)by;
            if (pictures || tracing) ImGui::Image((ImTextureID)(intptr_t)texture, ImVec2((float)shown_w * by, (float)shown_h * by));
            else ImGui::TextUnformatted("Render traces it.");
        }
        ImGui::End();
        ImGui::Render();
        glViewport(0, 0, (GLsizei)io.DisplaySize.x, (GLsizei)io.DisplaySize.y);
        glClearColor(0.08f, 0.09f, 0.11f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplShaderEmu_RenderDrawData(ImGui::GetDrawData());
        seglSwap();
        if (quit) break;

        if (stop) {
            // the shares end with the samples they are at (their cores must be free), and that is the picture
            pt.stop = 1;
            stopping = true;
            if (workers == 0) next = tiles, of_samples = sample + 1;
        }
        if (render || press) {
            press = false;
            take_workers(layout);
            pt.width = shown_w = sizes[size][0];
            pt.height = shown_h = sizes[size][1];
            pt.bounces = bounces;
            pt.material[0] = ball[0];
            pt.material[1] = ball[1];
            pt.scene = scene;
            pt.depth = getenv("NXPATH_DEPTH") != nullptr;
            pt.owners = owners;
            pt.light = light;
            pt.seed = 0x9e3779b9u;
            glBindTexture(GL_TEXTURE_2D, texture);
            seglTexturePointer(pt.pixels, pt.width, pt.height, GL_RGBA);
            memset(pt.pixels, 0, (size_t)(pt.width * pt.height * 4));
            if (workers) {
                // (what the workers will write and read, here before them)
                mcw_touch(pt.sums, (unsigned)(pt.width * pt.height) * 3 * sizeof(float));
                read_model(scene);
            }
            int s = 32;
            while (s > 1 && ((pt.width + (s < 8 ? 8 : s) - 1) / (s < 8 ? 8 : s)) * ((pt.height + s - 1) / s) < 24 * workers) s >>= 1;   // (this core alone: 32)
            pt.tile_h = s;
            pt.tile_w = s < 8 ? 8 : s;
            pt.tiles = tiles = ((pt.width + pt.tile_w - 1) / pt.tile_w) * ((pt.height + pt.tile_h - 1) / pt.tile_h);
            // a stride with no factor in common with the number of tiles visits every one of them
            for (pt.stride = (uint32_t)tiles * 5 / 8 + 1;; pt.stride++) {
                uint32_t a = pt.stride, b = (uint32_t)tiles;
                while (b) { uint32_t r = a % b; a = b; b = r; }
                if (a == 1) break;
            }
            pt.step = (uint32_t)(workers ? workers : 1) * pt.stride % (uint32_t)tiles;
            pt.samples = of_samples = samples;
            pt.stop = 0;
            stopping = false;
            memset(pt.slots, 0, MC_MAX_CORES * sizeof(struct pt_slot));
            memset(busy, 0, sizeof busy);
            sample = next = out = have = 0;
            rays = 0;
            began = now_ms();
            tracing = true;
        }
    }
    pt.stop = 1;   // (the shares end with the samples they are at: the workers are waited for)
    fprintf(stderr, "nxpath: done\n");
    GrClose();
    return 0;
}
