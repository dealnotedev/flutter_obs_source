#pragma once

#include <stdint.h>
#include <graphics/graphics.h>
#include "flutter_embedder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { FLUTTER_RENDERER_SOFTWARE, FLUTTER_RENDERER_GPU } flutter_renderer_type;
typedef struct flutter_renderer flutter_renderer;

/* Owns frame transfer and graphics resources. Destroy only after engine shutdown.
 * render must run in the OBS graphics context; all other calls may run outside it.
 * Pass this renderer as FlutterEngineInitialize's user_data. */
flutter_renderer *flutter_renderer_create(flutter_renderer_type type, uint32_t width, uint32_t height, void *owner);
void flutter_renderer_destroy(flutter_renderer *renderer);
FlutterRendererConfig flutter_renderer_config(flutter_renderer *renderer);
void *flutter_renderer_owner(void *renderer);
void flutter_renderer_resize(flutter_renderer *renderer, uint32_t width, uint32_t height);
void flutter_renderer_render(flutter_renderer *renderer, gs_effect_t *effect);

#ifdef __cplusplus
}
#endif
