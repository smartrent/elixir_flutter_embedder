#ifndef FLUTTER_EMBEDDER_GFX_H
#define FLUTTER_EMBEDDER_GFX_H

#include <stddef.h>
#include "flutter_embedder.h"

/**
 * Initialize whatever graphics backend is configured.
 * This is where `FlutterEngineRun()` should be called.
 * 
 * @param args partially complete (and already allocated) structure to pass to the engine
 * @return integer where 0 is ok, anything else is an error
 */
size_t gfx_init(FlutterEngine);

bool gfx_make_current(void*);
bool gfx_clear_current(void*);
bool gfx_present(void*);
uint32_t gfx_fbo_callback(void*);
bool runs_platform_tasks_on_current_thread(void *);
void on_post_flutter_task(FlutterTask, uint64_t, void *);
void gfx_vsync(void *,intptr_t);
void *proc_resolver(void *, const char *);

/**
 * Cleanup the graphics backend
 */
size_t gfx_terminate();

/**
 * Main graphics backend loop
 */
void gfx_loop();

#endif