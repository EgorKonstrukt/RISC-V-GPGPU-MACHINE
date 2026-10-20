/* OpenTTD's mixer (mixer.h) on the machine's sound card (docs/machine/sound.md): a channel is a
 * voice of the card, which mixes; a sound is copied into sound memory the first time it plays
 * and played from there. No samples are mixed by the CPU and no sound driver pulls any. */

#include "stdafx.h"
#include "core/math_func.hpp"
#include "mixer.h"
extern "C" {
#include "shaderemu_sound.h"
}

#include "safeguards.h"

struct MixerChannel {
	int voice;
	uint32_t address, length, kind, rate;
	uint32_t volume;
};

static MixerChannel _channels[8];
static int _channel_count;
static uint8_t _effect_vol;

/* sound memory: one pool, given out in turn, and what of it holds which sound */
static const uint32_t POOL = 1 << 20;
static uint8_t *_pool;
static uint32_t _pool_address, _pool_used;
struct Copied {
	std::shared_ptr<std::vector<std::byte>> memory;
	uint32_t address;
};
static std::vector<Copied> _copied;

bool MxInitialize(uint)
{
	if (_channel_count != 0) return true;
	if (snd_open() < 0) return false;
	_pool = static_cast<uint8_t *>(snd_memory(POOL, &_pool_address));
	if (_pool == nullptr) return false;
	for (MixerChannel &mc : _channels) {
		if ((mc.voice = snd_claim()) < 0) break;
		_channel_count++;
	}
	return _channel_count != 0;
}

void MxMixSamples(void *, uint) {}

MixerChannel *MxAllocateChannel()
{
	for (int i = 0; i < _channel_count; i++) {
		if (!snd_playing(_channels[i].voice)) return &_channels[i];
	}
	return nullptr;
}

/** Where a sound lies in sound memory, copied there now if it is not: 8-bit as the card's unsigned bytes. */
static uint32_t Place(const std::shared_ptr<std::vector<std::byte>> &mem, bool is16bit)
{
	for (const Copied &c : _copied) {
		if (c.memory == mem) return c.address;
	}
	uint32_t bytes = (static_cast<uint32_t>(mem->size()) + 3) & ~3u;
	if (bytes > POOL) return 0;
	if (_pool_used + bytes > POOL) {
		/* full: every voice stops and the pool is filled again from the start */
		for (int i = 0; i < _channel_count; i++) snd_stop(_channels[i].voice);
		_copied.clear();
		_pool_used = 0;
	}
	uint8_t *to = _pool + _pool_used;
	const uint8_t *from = reinterpret_cast<const uint8_t *>(mem->data());
	if (is16bit) {
		std::copy_n(from, mem->size(), to);
	} else {
		for (size_t i = 0; i < mem->size(); i++) to[i] = from[i] ^ 0x80;
	}
	_copied.push_back({ mem, _pool_address + _pool_used });
	_pool_used += bytes;
	return _copied.back().address;
}

void MxSetChannelRawSrc(MixerChannel *mc, const std::shared_ptr<std::vector<std::byte>> &mem, uint rate, bool is16bit)
{
	size_t samples = is16bit ? mem->size() / 2 : mem->size();
	mc->address = samples > 1 ? Place(mem, is16bit) : 0;
	mc->length = samples > 1 ? static_cast<uint32_t>(samples - 1) : 0;
	mc->kind = (is16bit ? SND_PCM16 : SND_PCM8) | SND_SMOOTH;
	mc->rate = rate;
}

void MxSetChannelVolume(MixerChannel *mc, uint volume, float pan)
{
	/* as mixer.cpp mixes: a sinusoidal pan, then the effects' volume cubed; 65,536 is the card's 256 */
	uint effect = _effect_vol * _effect_vol * _effect_vol / (127 * 127);
	uint left = static_cast<uint>(sin((1.0 - pan) * M_PI / 2.0) * volume) * effect / 255;
	uint right = static_cast<uint>(sin(pan * M_PI / 2.0) * volume) * effect / 255;
	mc->volume = SND_VOLUME(std::min(left >> 8, 256u), std::min(right >> 8, 256u));
}

void MxActivateChannel(MixerChannel *mc)
{
	if (mc->address == 0) return;
	uint32_t step = snd_step(mc->rate);
	if (step == 0) step = static_cast<uint32_t>((static_cast<uint64_t>(mc->rate) << 16) / 48000);
	snd_play(mc->voice, mc->kind, mc->address, mc->length, 0, step, mc->volume);
}

void MxCloseAllChannels()
{
	for (int i = 0; i < _channel_count; i++) snd_stop(_channels[i].voice);
}

uint32_t MxSetMusicSource(MxStreamCallback)
{
	return 0;
}

void SetEffectVolume(uint8_t volume)
{
	_effect_vol = volume;
}
