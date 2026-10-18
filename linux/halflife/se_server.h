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

// fn(args) made by the first core, the worker waiting for it; called directly elsewhere.
uint32_t SE_ServerOnCore0( uint32_t ( *fn )( void *args ), void *args );
void SE_ServerKeepPrint( const char *text );
// A packet the loopback sends kept for later (1), while the server's frame is out.
int SE_ServerKeepPacket( int sock, size_t length, const void *data, const void *to );
// An error in the server's frame on its worker: the frame ends and it is raised by the first core.
void SE_ServerAbort( const char *text, int fatal ) __attribute__(( noreturn ));
void SE_ServerSettle( void );
// What the worker asks of the first core made now, if it asks: between the first core's own steps.
void SE_ServerServe( void );
int SE_ServerGive( void );
#endif
