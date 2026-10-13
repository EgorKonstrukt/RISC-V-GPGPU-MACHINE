// The machine's GPU for Quake III Arena (docs/quake3.md): programs/linux/gles.c as it is (its
// window, its frames' two sets, its matrices, its swap), and after it draws whose every word
// the renderer says itself: its textures are in the ROM, which the library's own have no way
// to say, and a draw may be lit by a light map (docs/gpu.md).

// A frame of this game is more vertices than the library's default has room for.
#define SET_SIZE 0x00080000u
#define UNIFORMS_IN 0x00030000u
#define VERTICES_IN 0x00038000u
#define MAX_COMMANDS 3072
#include "gles.c"
#include "gpu_shaderemu.h"

const volatile unsigned* gpuControl(int offset) {
    return gpu ? (const volatile unsigned*)(gpu + offset) : 0;
}

// Where the window's first pixel is on the display: its place in each window it is inside.
void gpuWindowOrigin(int* x, int* y) {
    GR_WINDOW_INFO info;
    *x = *y = 0;
    for (GR_WINDOW_ID id = window; gpu && id; id = info.parent) {
        GrGetWindowInfo(id, &info);
        *x += info.x, *y += info.y;
    }
}

unsigned gpuAddress(const void* memory) { return GPU_PHYS + (uint32_t)((const uint8_t*)memory - gpu); }

void gpuProjection(const float* columns) {
    matrix_mode = GL_PROJECTION;
    load_columns(columns, 0);
    matrix_mode = GL_MODELVIEW;
}
void gpuModelview(const float* columns) { load_columns(columns, 0); }

void gpuModelviewRows(const float* rows) {
    matrix* m = &modelview[modelview_top];
    for (int i = 0; i < 12; i++) m->r[i / 4][i % 4] = rows[i];
    // the block the draws that follow read: the one before with these rows in it (its other
    // words, the projection with the window folded in, are the same)
    if (matrices_dirty || !blocks || blocks >= MAX_BLOCKS) {
        matrices_dirty = 1;
        return;
    }
    num* u = (num*)(gpu + set_at + UNIFORMS_IN) + 32 * blocks;
    for (int i = 0; i < 16; i++) u[i] = u[i - 32];
    for (int i = 0; i < 12; i++) u[16 + i] = rows[i];
    blocks++;
}

// The words of a draw's command but for where its vertices are, how many, and its matrices.
static inline void command_state(uint32_t* c, const gpu_state* s) {
    c[4] = VERTEX_CLIP | VERTEX_MODELVIEW | VERTEX_COMPACT | VERTEX_FLOAT | s->vertex;
    c[5] = s->fragment;
    c[7] = s->texture;
    c[8] = s->width;
    c[9] = s->height;
    c[10] = s->key;
    c[11] = s->colour;
    c[12] = s->more[0], c[13] = s->more[1], c[14] = s->more[2], c[15] = s->more[3];
}

static gpu_state joined;      // what the frame's last command was made with, when more may join it
static uint32_t* joined_command;
static uint32_t joined_block, joined_end;   // its matrices, and where its vertices end

static inline int same_state(const gpu_state* a, const gpu_state* b) {
    const uint32_t *x = (const uint32_t*)a, *y = (const uint32_t*)b;
    uint32_t differ = 0;
    for (unsigned i = 0; i < sizeof(gpu_state) / 4; i++) differ |= x[i] ^ y[i];
    return !differ;
}

unsigned gpuMemoryMark(void) { return texture_top; }
void gpuMemoryRelease(unsigned mark) {
    seglSettle();   // (the list being drawn may read what is given back and written again)
    texture_top = mark;
}

unsigned* gpuDrawAt(const gpu_state* s, unsigned vertices, int mesh) {
    uint32_t block = matrix_block();
    uint32_t* c = command(CMD_DRAW, (uint32_t)mesh);
    if (!c) return 0;
    c[1] = vertices;
    c[2] = (uint32_t)mesh;
    c[6] = block;
    command_state(c, s);
    passes_used |= (1u << (s->fragment >> 16 & 7)) & 0xfe;
    last_draw = 0;
    joined_command = 0;
    return c;
}

unsigned* gpuDrawRoom(const gpu_state* s, int stored, int mesh) {
    if (vertex_top + (uint32_t)stored * 16 > set_at + SET_SIZE || mesh_vertices + (uint32_t)mesh > MAX_MESH) return 0;
    uint32_t* to = (uint32_t*)(gpu + vertex_top);
    uint32_t block = matrices_dirty || !blocks ? matrix_block() : GPU_PHYS + set_at + UNIFORMS_IN + 128 * (blocks - 1);
    if (joined_command && joined_command == (uint32_t*)(gpu + set_at) + 16 * (commands - 1) && block == joined_block &&
        joined_end == vertex_top && same_state(s, &joined)) {
        joined_command[2] += (uint32_t)mesh;
        mesh_vertices += (uint32_t)mesh;
    } else {
        uint32_t* c = command(CMD_DRAW, (uint32_t)mesh);
        if (!c) return 0;
        c[1] = GPU_PHYS + vertex_top;
        c[2] = (uint32_t)mesh;
        c[6] = block;
        command_state(c, s);
        passes_used |= (1u << (s->fragment >> 16 & 7)) & 0xfe;
        joined = *s, joined_command = c, joined_block = block;
        last_draw = 0;
    }
    vertex_top += (uint32_t)stored * 16;
    joined_end = vertex_top;
    return to;
}

void* gpuFrameMemory(unsigned bytes) {
    bytes = (bytes + 15) & ~15u;
    if (!gpu || vertex_top + bytes > set_at + SET_SIZE) return 0;
    void* at = gpu + vertex_top;
    vertex_top += bytes;
    joined_command = 0;   // (what follows is not next to the last command's vertices)
    return at;
}

int gpuKeep(int count, int mesh) {
    if (!gpu || count < 0 || count > MAX_COMMANDS - 64 || (uint32_t)mesh > MAX_MESH - 4096) return -1;
    seglSettle();
    kept = (uint32_t)count;
    kept_passes = passes_used = 0;
    kept_mesh = (uint32_t)mesh;
    commands = kept;
    mesh_vertices = kept_mesh;
    last_draw = 0;
    joined_command = 0;
    blocks = 0;
    return 0;
}

unsigned* gpuKept(int set, int index, const gpu_state* s, unsigned vertices, int first_mesh, int mesh) {
    uint32_t* c = kept_command(set, index);
    c[0] = CMD_DRAW;
    c[1] = vertices;
    c[2] = (uint32_t)mesh;
    c[3] = (uint32_t)first_mesh;
    c[6] = GPU_PHYS + SETS_AT + (uint32_t)set * SET_SIZE + UNIFORMS_IN;
    command_state(c, s);
    kept_passes |= (1u << (s->fragment >> 16 & 7)) & 0xfe;
    passes_used |= kept_passes;
    return c;
}

unsigned* gpuKeptCommand(int index) { return kept_command(set_at != SETS_AT, index); }

void gpuSwap(int wait) {
    joined_command = 0;
    swap(wait);
}
