/* OpenTTD's screen in a Nano-X window, drawn by the machine's GPU (docs/games/openttd.md). */

#ifndef VIDEO_SHADEREMU_H
#define VIDEO_SHADEREMU_H

#include "video_driver.hpp"

class VideoDriver_ShaderEmu : public VideoDriver {
public:
	std::optional<std::string_view> Start(const StringList &param) override;
	void Stop() override;
	void MakeDirty(int left, int top, int width, int height) override;
	void MainLoop() override;
	bool ChangeResolution(int w, int h) override;
	bool ToggleFullscreen(bool fullscreen) override;
	bool AfterBlitterChange() override;
	bool HasEfficient8Bpp() const override { return true; }
	std::string_view GetName() const override { return "shaderemu"; }

protected:
	Dimension GetScreenSize() const override;
	void InputLoop() override;
	bool LockVideoBuffer() override;
	void UnlockVideoBuffer() override;
	void Paint() override;
	void CheckPaletteAnim() override;
	bool PollEvent() override;

private:
	Palette local_palette{};
	Rect dirty_rect{};
	bool Resize(int w, int h);
};

class FVideoDriver_ShaderEmu : public DriverFactoryBase {
public:
	FVideoDriver_ShaderEmu() : DriverFactoryBase(Driver::DT_VIDEO, 10, "shaderemu", "ShaderEmu Nano-X Video Driver") {}
	std::unique_ptr<Driver> CreateInstance() const override { return std::make_unique<VideoDriver_ShaderEmu>(); }
};

#endif /* VIDEO_SHADEREMU_H */
