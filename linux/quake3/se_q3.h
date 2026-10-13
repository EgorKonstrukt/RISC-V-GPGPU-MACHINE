/*
 * What the port's own files say to each other (docs/quake3.md): the system layer, the
 * renderer, the sound and the GPU library.
 */
#ifndef SHADEREMU_QUAKE3_H
#define SHADEREMU_QUAKE3_H

#include <stdint.h>

/* gpu_shaderemu.c: gles.c and what the renderer needs beside it */
const volatile unsigned *gpuControl (int offset);	/* the machine's control words; NULL before the window */
#define REG_INPUT	0x20		/* the machine's input counters (docs/input.md) */
#define REG_CLOCK	0x34		/* its clock, in milliseconds */

/* sys_shaderemu.c */
unsigned SE_Cycles (void);		/* instructions this core has run */
extern unsigned se_started;		/* that count at main */
unsigned SE_RomAddress (int fd, const char *begins, int count);	/* an open file's place in the ROM, 0 for none */
/* bake.py's file (se_data.h), mapped: its first byte, and where that is in the ROM; one of its tables */
#define SE_DATA_FILE	"/usr/share/quake3-se.dat"
const unsigned char *SE_Data (unsigned *rom);
const void *SE_DataSection (const char *tag, unsigned each, int *count);

/*
 * The part of a frame that begins here, and the one it was (to go back to): 0 the rest, 1 the
 * server, 2 the client and its game, 3 a scene, 4 the swap, 5 the sound.
 */
int SE_Phase (int next);
void SE_FrameDone (void);		/* a frame was shown: the q3stat: lines, QUAKE3_HOLD */

/* QUAKE3_WANTED=1: a line for every file, model, picture and sound the game asks for (tools: bake.py's list) */
extern int se_wanted;
void SE_Wanted (const char *kind, const char *name);

/* server_shaderemu.c: the server's frame on a worker core (qcommon.h has what the engine asks of it) */
extern unsigned se_main_stack, se_server_jobs, se_server_cycles, se_server_passes;
void SE_ServerPoll (void);

/* tr_shaderemu.c */
int SE_ClientPlaying (void);		/* the client of this machine is in a game (not loading one, not at a menu) */
void SE_Settle (void);			/* waits until the GPU has drawn the frame last shown */
void SE_Input (void);			/* the window's keys and the pointer, as the game's events */
unsigned SE_WindowClock (void);		/* the machine's clock once the window is open, else 0 */

#endif
