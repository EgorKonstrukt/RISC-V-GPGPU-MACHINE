// The machine's GPU for Quake (docs/quake.md): programs/linux/gles.c as it is, and after it
// what a thing drawn again frame after frame asks of it: its matrices as rows, and a draw made
// from the words of the command that drew it before (qgl.c keeps those).

#include "gles.c"

// How many commands the frame has so far, and the last of them: a draw of vertices that are
// in GPU memory already makes one of its own, which qgl.c copies.
unsigned qgpuCommands(void) { return commands; }
const unsigned* qgpuLastCommand(void) { return commands ? (const uint32_t*)(gpu + set_at) + 16 * (commands - 1) : 0; }

// The modelview's three rows that draws read, set as they are: the block of matrices the next
// draws read is the one before with these rows in it (its projection is the same), unless the
// library has a block to write anyway.
void qgpuRows(const float* rows) {
    matrix* m = &modelview[modelview_top];
    for (int i = 0; i < 12; i++) m->r[i / 4][i % 4] = rows[i];
    if (matrices_dirty || !blocks || blocks >= MAX_BLOCKS) {
        matrices_dirty = 1;
        return;
    }
    num* u = (num*)(gpu + set_at + UNIFORMS_IN) + 32 * blocks;
    for (int i = 0; i < 16; i++) u[i] = u[i - 32];
    for (int i = 0; i < 12; i++) u[16 + i] = rows[i];
    blocks++;
}

// A draw from the sixteen words of a command that drew the same before, with the matrices in
// force now. -1 when the frame is full.
int qgpuReplay(const unsigned* words) {
    uint32_t block = matrix_block();
    uint32_t* c = command(CMD_DRAW, words[2]);
    if (!c) return -1;
    c[1] = words[1], c[2] = words[2], c[4] = words[4], c[5] = words[5], c[6] = block, c[7] = words[7];
    c[8] = words[8], c[9] = words[9], c[10] = words[10], c[11] = words[11];
    c[12] = words[12], c[13] = words[13], c[14] = words[14], c[15] = words[15];
    passes_used |= (1u << (words[5] >> 16 & 7)) & 0xfe;
    last_draw = 0;
    return 0;
}

// A frame's first draws that are the same frame after frame (the status bar): where they
// begin (words 0-3 of `kept`), how much of the frame they are (4-7) and its passes (8), noted
// round them when they are drawn; and counted, not written, in a later frame of the same set
// that begins the same, where the commands, vertices and matrices still lie as they were left.
void qgpuHeadBegin(unsigned* kept) {
    kept[0] = commands, kept[1] = vertex_top - set_at, kept[2] = mesh_vertices, kept[3] = blocks;
}
void qgpuHeadEnd(unsigned* kept) {
    kept[4] = commands - kept[0], kept[5] = vertex_top - set_at - kept[1], kept[6] = mesh_vertices - kept[2];
    kept[7] = blocks - kept[3], kept[8] = passes_used;
    last_draw = 0;
}
int qgpuHeadAgain(const unsigned* kept) {
    if (commands != kept[0] || vertex_top - set_at != kept[1] || mesh_vertices != kept[2] || blocks != kept[3]) return 0;
    commands += kept[4], vertex_top += kept[5], mesh_vertices += kept[6], blocks += kept[7];
    passes_used |= kept[8];
    matrices_dirty = 1;   // (the matrices in force are not the last block's)
    last_draw = 0;
    return 1;
}
