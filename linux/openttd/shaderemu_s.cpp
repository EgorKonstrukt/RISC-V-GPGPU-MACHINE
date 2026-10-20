/* The machine's sound card as OpenTTD's sound driver (shaderemu_s.h). */

#include "../stdafx.h"
#include "../mixer.h"
#include "shaderemu_s.h"

#include "../safeguards.h"

static FSoundDriver_ShaderEmu iFSoundDriver_ShaderEmu;

std::optional<std::string_view> SoundDriver_ShaderEmu::Start(const StringList &)
{
	if (!MxInitialize(48000)) return "no sound card";
	return std::nullopt;
}

void SoundDriver_ShaderEmu::Stop()
{
	MxCloseAllChannels();
}
