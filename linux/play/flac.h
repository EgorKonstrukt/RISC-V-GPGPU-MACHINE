/* A FLAC file in memory (flac.c, over dr_flac): one at a time. */
#ifndef NXPLAY_FLAC_H
#define NXPLAY_FLAC_H

/* True if it is one; `frames` is 0 when the file does not say how long it is. */
int flac_open(const void *file, unsigned size, int *rate, int *channels, unsigned *frames);
/* The next frames, as many 16-bit samples each as the file has channels; fewer at the end. */
int flac_read(short *to, int frames);
/* Where it then is. */
unsigned flac_seek(unsigned frame);
void flac_close(void);

/* For the worker cores, after flac_open: true when its blocks are all as long (and how long),
 * so that each can be decoded by itself. */
int flac_blocks(const void *file, unsigned size, unsigned *frames_a_block);
/* Where a block begins in the file; 0 while it is not found (`most` bytes are looked through a call). */
unsigned flac_block_at(unsigned block, unsigned most);
/* Decoder n (one a core), made by the first core. Then, on any core: it goes to the block
 * that begins at `at`, and gives the frames from there on. */
int flac_decoder(int n);
void flac_from(int n, unsigned at);
int flac_next(int n, short *to, int frames);

#endif
