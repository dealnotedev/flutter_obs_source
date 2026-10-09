#include "flutter-renderer-internal.hpp"
#include <obs-module.h>
#include <array>
#include <cstring>
#include <vector>

namespace {
enum class SlotState { free, writing, ready, reading };
struct SoftwareFrame {
	std::vector<uint8_t> pixels;
	uint32_t width = 0;
	uint32_t height = 0;
	SlotState state = SlotState::free;
};

class SoftwareRenderer final : public flutter_renderer {
public:
	using flutter_renderer::flutter_renderer;
	~SoftwareRenderer() override
	{
		obs_enter_graphics();
		gs_texture_destroy(display);
		obs_leave_graphics();
	}

	FlutterRendererConfig config() override
	{
		FlutterRendererConfig result = {};
		result.type = kSoftware;
		result.software.struct_size = sizeof(result.software);
		result.software.surface_present_callback = present;
		return result;
	}

	gs_texture_t *texture() override
	{
		int index;
		uint32_t requested_width, requested_height;
		{
			std::lock_guard<std::mutex> lock(mutex);
			requested_width = width;
			requested_height = height;
			index = ready;
			if (index >= 0) {
				frames[index].state = SlotState::reading;
				ready = -1;
			}
		}
		if (index >= 0) {
			auto &frame = frames[index];
			if (!display || gs_texture_get_width(display) != frame.width ||
			    gs_texture_get_height(display) != frame.height) {
				gs_texture_destroy(display);
				display = gs_texture_create(frame.width, frame.height, GS_BGRA, 1, nullptr, GS_DYNAMIC);
			}
			if (display)
				gs_texture_set_image(display, frame.pixels.data(), frame.width * 4, false);
			std::lock_guard<std::mutex> lock(mutex);
			frame.state = SlotState::free;
		}
		if (display && (gs_texture_get_width(display) != requested_width ||
				gs_texture_get_height(display) != requested_height)) {
			gs_texture_destroy(display);
			display = nullptr;
		}
		return display;
	}

private:
	void invalidate_ready() override
	{
		if (ready >= 0)
			frames[ready].state = SlotState::free;
		ready = -1;
	}
	static bool present(void *user_data, const void *allocation, size_t row_bytes, size_t height)
	{
		auto &self = *static_cast<SoftwareRenderer *>(static_cast<flutter_renderer *>(user_data));
		if (!allocation)
			return false;
		int index = -1;
		uint32_t width;
		uint64_t generation;
		{
			std::lock_guard<std::mutex> lock(self.mutex);
			width = self.width;
			generation = self.generation;
			if (height != self.height || row_bytes < size_t(width) * 4 ||
			    (height && row_bytes > SIZE_MAX / height) || height > SIZE_MAX / (size_t(width) * 4))
				return true;
			for (size_t i = 0; i < self.frames.size(); ++i) {
				if (self.frames[i].state == SlotState::free) {
					index = int(i);
					self.frames[i].state = SlotState::writing;
					break;
				}
			}
		}
		if (index < 0)
			return true;
		auto &frame = self.frames[index];
		try {
			frame.pixels.resize(size_t(width) * 4 * height);
		} catch (const std::exception &error) {
			blog(LOG_ERROR, "[FlutterSource] Cannot allocate software frame: %s", error.what());
			std::lock_guard<std::mutex> lock(self.mutex);
			frame.state = SlotState::free;
			return false;
		}
		for (size_t row = 0; row < height; ++row)
			memcpy(frame.pixels.data() + row * width * 4,
			       static_cast<const uint8_t *>(allocation) + row * row_bytes, size_t(width) * 4);
		{
			std::lock_guard<std::mutex> lock(self.mutex);
			frame.state = SlotState::free;
			if (generation == self.generation) {
				self.invalidate_ready();
				frame.width = width;
				frame.height = uint32_t(height);
				frame.state = SlotState::ready;
				self.ready = index;
			}
		}
		return true;
	}
	std::array<SoftwareFrame, 3> frames;
	int ready = -1;
	gs_texture_t *display = nullptr;
};
} // namespace

flutter_renderer *flutter_software_renderer_create(uint32_t width, uint32_t height, void *owner)
{
	return new SoftwareRenderer(width, height, owner);
}
