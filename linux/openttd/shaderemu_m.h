/* Music on the machine's sound card (docs/machine/sound.md, kind 5): a song becomes tracks of
 * notes for FM voices when it starts, played on DMXOPL's instruments; the card does the rest. */

#ifndef MUSIC_SHADEREMU_H
#define MUSIC_SHADEREMU_H

#include "music_driver.hpp"

class MusicDriver_ShaderEmu : public MusicDriver {
public:
	std::optional<std::string_view> Start(const StringList &param) override;
	void Stop() override;
	void PlaySong(const MusicSongInfo &song) override;
	void StopSong() override;
	bool IsSongPlaying() override;
	void SetVolume(uint8_t vol) override;
	std::string_view GetName() const override { return "shaderemu"; }
};

class FMusicDriver_ShaderEmu : public DriverFactoryBase {
public:
	FMusicDriver_ShaderEmu() : DriverFactoryBase(Driver::DT_MUSIC, 10, "shaderemu", "ShaderEmu FM Music Driver") {}
	std::unique_ptr<Driver> CreateInstance() const override { return std::make_unique<MusicDriver_ShaderEmu>(); }
};

#endif /* MUSIC_SHADEREMU_H */
