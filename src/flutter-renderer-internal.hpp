#pragma once

#include "flutter-renderer.h"
#include <mutex>
#include <utility>

struct flutter_renderer {
	flutter_renderer(uint32_t width, uint32_t height, void *owner) : width(width), height(height), owner(owner) {}
	virtual ~flutter_renderer() = default;
	virtual FlutterRendererConfig config() = 0;
	/* Returns a retained display texture, owned exclusively by the graphics thread. */
	virtual gs_texture_t *texture() = 0;
	virtual uint32_t draw_flags() const { return 0; }
	void resize(uint32_t new_width, uint32_t new_height)
	{
		std::lock_guard<std::mutex> lock(mutex);
		width = new_width;
		height = new_height;
		++generation;
		invalidate_ready();
	}
	std::pair<uint32_t, uint32_t> size()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return {width, height};
	}

	std::mutex mutex;
	uint32_t width;
	uint32_t height;
	uint64_t generation = 1;
	void *owner;

protected:
	/* Called while mutex is held, after the requested dimensions change. */
	virtual void invalidate_ready() = 0;
};

flutter_renderer *flutter_software_renderer_create(uint32_t width, uint32_t height, void *owner);
flutter_renderer *flutter_gpu_renderer_create(uint32_t width, uint32_t height, void *owner);
