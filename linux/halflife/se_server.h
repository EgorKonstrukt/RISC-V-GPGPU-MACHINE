// The server's frame on a worker core (sv_shaderemu.c, docs/games/halflife.md).
#ifndef SE_SERVER_H
#define SE_SERVER_H
#include <stddef.h>
#include <stdint.h>

#define SE_SERVER_STACK 0x80000
#define SE_APART __attribute__(( aligned( 16 )))	// (what each core writes of its own: 16 bytes apart)
extern char se_server_stack[SE_SERVER_STACK];

// Whether the code that asks runs in the server's frame on its worker.
static inline int SE_OnServer( void )
{
	char here;
	return (uint32_t)( &here - se_server_stack ) < SE_SERVER_STACK;
}

#define SE_RENDER_STACK 0x40000
extern char se_render_stack[SE_RENDER_STACK];

// Whether the code that asks runs in the frame's drawing on its worker (rnd_shaderemu.c).
static inline int SE_OnRender( void )
{
	char here;
	return (uint32_t)( &here - se_render_stack ) < SE_RENDER_STACK;
}
// Whose of the cores' own copies the code that asks uses: the first core's 0, the server's 1, the drawing's 2.
static inline int SE_Who( void ) { return SE_OnServer() ? 1 : SE_OnRender() ? 2 : 0; }
static inline int SE_OffCore0( void ) { return SE_OnServer() || SE_OnRender(); }

// fn(args) made by the first core, the worker waiting for it; called directly elsewhere.
uint32_t SE_ServerOnCore0( uint32_t ( *fn )( void *args ), void *args );
void SE_ServerKeepPrint( const char *text );
// A packet the loopback sends kept for later (1), while the server's frame is out.
int SE_ServerKeepPacket( int sock, size_t length, const void *data, const void *to );
// The next of the client's packets kept while the frame was out (1), read by the server before the loopback's.
int SE_ServerTakePacket( void *data, size_t *length );
// An error in the server's frame on its worker: the frame ends and it is raised by the first core.
void SE_ServerAbort( const char *text, int fatal ) __attribute__(( noreturn ));
void SE_ServerSettle( void );
// What the workers ask of the first core made now, if they ask: between the first core's own steps.
void SE_ServerServe( void );
uint32_t SE_RenderOnCore0( uint32_t ( *fn )( void *args ), void *args );
void SE_RenderKeepPrint( const char *text );
void SE_RenderAbort( const char *text, int fatal ) __attribute__(( noreturn ));
void SE_RenderServe( void );
int SE_ServerGive( void );
// The time the server's frame covers: on its worker, the host's frames since the last was given.
double SE_ServerFrametime( void );
// At a host frame's start: the frame out ended, unless it is paced and not done yet.
void SE_ServerFrameStart( void );
// Whether this core is making what the server's worker asked for (it waits meanwhile).
int SE_ServerServing( void );
#endif
