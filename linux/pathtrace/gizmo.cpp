// The handles of what is selected (scene.h): ImGuizmo, drawn by Dear ImGui with no window of
// the program's in it. The pointer is the caller's: the backend's NewFrame, which takes every
// event the program has, is not used.
#include "imgui.h"
#include "imgui_impl_shaderemu.h"
#include "ImGuizmo.h"
extern "C" {
#include <GLES/segl.h>
#include "scene.h"
}

#define HANDLE 80.0f    // an axis's length in pixels, whatever the picture's size

static unsigned last_ms;

// The machine's clock in milliseconds (docs/input.md).
static unsigned now_ms() { return *(const volatile unsigned*)((const char*)seglPalette() - 0x400 + 0x34); }

// ImGuizmo's sines and cosines (build.sh names them for it): the C library's are worked out
// in doubles, which the machine has not, and a turning handle asks for 400 a frame.
// Quarter turns off, then a series: right to the sixth place.
static float sine(float angle, int quarter)
{
    const int turns = (int)(angle * 0.63661977f + (angle < 0 ? -0.5f : 0.5f));
    const float r = angle - (float)turns * 1.57079633f, s = r * r;
    quarter += turns;
    const float value = quarter & 1 ? 1 + s * (-0.5f + s * (0.041666667f + s * (-0.0013888889f + s * 0.0000248016f)))
                                    : r * (1 + s * (-0.16666667f + s * (0.0083333333f + s * -0.00019841270f)));
    return quarter & 2 ? -value : value;
}

extern "C" float gizmo_sin(float angle) { return sine(angle, 0); }
extern "C" float gizmo_cos(float angle) { return sine(angle, 1); }

int gizmo_init(unsigned int window)
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    if (!ImGui_ImplShaderEmu_Init(window)) return 0;
    // ImGui smooths a line by a texture's soft edge, and the device takes the nearest texel: the
    // same pixels as a plain line, whose quads the backend copies in half the instructions.
    ImGui::GetStyle().AntiAliasedLines = false;
    last_ms = now_ms();
    return 1;
}

int gizmo_frame(const float* view, const float* projection, float* matrix, int what, int width, int height, int x, int y, int buttons)
{
    if (width <= 0 || height <= 0) return 0;
    ImGuiIO& io = ImGui::GetIO();
    const unsigned now = now_ms();
    io.DisplaySize = ImVec2((float)width, (float)height);
    io.DeltaTime = now != last_ms ? (float)(now - last_ms) * 0.001f : 0.001f;
    last_ms = now;
    io.AddMousePosEvent((float)x, (float)y);
    io.AddMouseButtonEvent(0, (buttons & 1) != 0);
    ImGui::NewFrame();
    // (its own window, which takes no input: the handles are drawn into that window's list)
    ImGuizmo::BeginFrame();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(0, 0, (float)width, (float)height);
    ImGuizmo::SetGizmoSizeClipSpace(2.0f * HANDLE / (float)width);
    const ImGuizmo::OPERATION operation = what == GIZMO_TURN ? ImGuizmo::ROTATE : what == GIZMO_SIZE ? ImGuizmo::SCALE : ImGuizmo::TRANSLATE;
    float before[16];
    memcpy(before, matrix, sizeof before);
    // moved along the world's axes, turned about the thing's own (and sized along them, always)
    ImGuizmo::Manipulate(view, projection, operation, what == GIZMO_MOVE ? ImGuizmo::WORLD : ImGuizmo::LOCAL, matrix);
    const bool held = ImGuizmo::IsOver(operation);
    ImGui::Render();
    ImGui_ImplShaderEmu_RenderDrawData(ImGui::GetDrawData());
    return memcmp(before, matrix, sizeof before) ? 1 : held ? 2 : 0;
}
