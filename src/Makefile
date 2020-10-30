PREFIX = $(MIX_APP_PATH)/priv
BUILD  = $(MIX_APP_PATH)/obj

SRC = erlcmd.c

ifeq ($(CROSSCOMPILE),)
# Not Crosscompiled build
SRC += embedder_glfw.c
# glfw
CFLAGS := -I/usr/include/GLFW/ -I/usr/include/GL/
LDFLAGS := -L$(PREFIX) -lGLEW -lX11 -lGLU -lGL -lglfw -lflutter_engine -lpthread
else
# Crosscompiled build
SRC += embedder_drm.c
# drm
CFLAGS += $(shell pkg-config libdrm --cflags)
CFLAGS += -DBUILD_ERLCMD_PLUGIN -Wall

LDFLAGS += $(shell pkg-config libdrm --libs)
# LDFLAGS += -ldrm  -ldl -lgbm -lGLESv2 -lEGL -lglfw -pthread
LDFLAGS += -ldrm  -ldl -lgbm -lGLESv2 -lEGL -pthread
LDFLAGS += -L$(PREFIX) -lflutter_engine
endif

all: $(PREFIX)/flutter_embedder

$(PREFIX)/flutter_embedder: $(PREFIX) $(PREFIX)/libflutter_engine.so $(PREFIX)/icudtl.dat flutter_embedder.h ${SRC}
	$(CC) ${SRC} ${CFLAGS} $(LDFLAGS) -o $(PREFIX)/flutter_embedder

ifeq ($(CROSSCOMPILE),)
include precompiled_engine_x86_64.mk
else
include precompiled_engine_armv7.mk
endif

format:
	astyle --style=kr --indent=spaces=4 --align-pointer=name \
	    --align-reference=name --convert-tabs --attach-namespaces \
	    --max-code-length=110 --max-instatement-indent=120 --pad-header \
	    --pad-oper \
	    embedder_drm.c

clean:
	rm -rf $(PREFIX)/libflutter_engine.so flutter_embedder.h $(PREFIX)/flutter_embedder *.zip

$(PREFIX) $(BUILD):
	mkdir -p $(PREFIX)

.PHONY: all clean format
