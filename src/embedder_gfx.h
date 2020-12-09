#ifndef FLUTTER_EMBEDDER_GFX_H
#define FLUTTER_EMBEDDER_GFX_H

#include <stddef.h>
#include "flutter_embedder.h"

/**
 * Initialize whatever graphics backend is configured
 * 
 * @param args partially complete (and already allocated) structure to pass to the engine
 * @return integer where 0 is ok, anything else is an error
 */
size_t gfx_init(FlutterProjectArgs*);

/**
 * Cleanup the graphics backend
 */
size_t gfx_terminate();

/**
 * Main graphics backend loop
 */
void gfx_loop();

#endif