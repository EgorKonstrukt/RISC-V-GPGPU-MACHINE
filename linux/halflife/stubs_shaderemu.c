/*
stubs_shaderemu.c - what the image has no use for (docs/games/halflife.md): voice chat (Opus)
*/
#include "common.h"
#include "client.h"
#include "voice.h"
#include "soundlib.h"

voice_state_t voice;
CVAR_DEFINE_AUTO( voice_loopback, "0", FCVAR_PRIVILEGED, "loop voice back (no voice on this machine)" );

void CL_AddVoiceToDatagram( void ) { }
void Voice_RegisterCvars( void ) { Cvar_RegisterVariable( &voice_loopback ); }
qboolean Voice_Init( const char *pszCodecName, int quality, qboolean preinit ) { return false; }
void Voice_Idle( double frametime ) { }
qboolean Voice_IsRecording( void ) { return false; }
void Voice_RecordStop( void ) { }
void Voice_RecordStart( void ) { }
void Voice_Disconnect( void ) { }
void Voice_AddIncomingData( int ent, const byte *data, uint size, uint frames ) { }
void Voice_StopChannel( int entnum ) { }
void Voice_LoopbackAck( void ) { }
voice_audio_info_t Voice_GetAudioInfo( void )
{
	voice_audio_info_t none;
	memset( &none, 0, sizeof( none ));
	return none;
}

