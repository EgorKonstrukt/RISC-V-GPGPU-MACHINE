/* The machine's sound card (docs/machine/sound.md): it mixes, so this only switches it on;
 * mixer_shaderemu.cpp plays each sound on a voice of it. */

#ifndef SOUND_SHADEREMU_H
#define SOUND_SHADEREMU_H

#include "sound_driver.hpp"

class SoundDriver_ShaderEmu : public SoundDriver {
public:
	std::optional<std::string_view> Start(const StringList &param) override;
	void Stop() override;
	std::string_view GetName() const override { return "shaderemu"; }
};

class FSoundDriver_ShaderEmu : public DriverFactoryBase {
public:
	FSoundDriver_ShaderEmu() : DriverFactoryBase(Driver::DT_SOUND, 10, "shaderemu", "ShaderEmu Sound Card Driver") {}
	std::unique_ptr<Driver> CreateInstance() const override { return std::make_unique<SoundDriver_ShaderEmu>(); }
};

#endif /* SOUND_SHADEREMU_H */
