#include "flutter-renderer-internal.hpp"
#include <obs-module.h>
#include <new>

flutter_renderer *flutter_renderer_create(flutter_renderer_type type, uint32_t width, uint32_t height, void *owner)
{
	if (!width || !height)
		return nullptr;
	try {
		return type == FLUTTER_RENDERER_GPU ? flutter_gpu_renderer_create(width, height, owner)
						    : flutter_software_renderer_create(width, height, owner);
	} catch (const std::exception &error) {
		blog(LOG_ERROR, "[FlutterSource] Renderer creation failed: %s", error.what());
		return nullptr;
	}
}

void flutter_renderer_destroy(flutter_renderer *renderer)
{
	delete renderer;
}

FlutterRendererConfig flutter_renderer_config(flutter_renderer *renderer)
{
	return renderer->config();
}

void *flutter_renderer_owner(void *renderer)
{
	return static_cast<flutter_renderer *>(renderer)->owner;
}

void flutter_renderer_resize(flutter_renderer *renderer, uint32_t width, uint32_t height)
{
	renderer->resize(width, height);
}

void flutter_renderer_render(flutter_renderer *renderer, gs_effect_t *effect)
{
	gs_texture_t *texture = renderer->texture();
	if (!texture || !effect)
		return;
	const auto [width, height] = renderer->size();
	const bool srgb_previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	if (image) {
		gs_effect_set_texture_srgb(image, texture);
		gs_draw_sprite(texture, renderer->draw_flags(), width, height);
	}
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(srgb_previous);
}
