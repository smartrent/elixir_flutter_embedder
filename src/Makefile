PREFIX = $(MIX_APP_PATH)/priv
BUILD  = $(MIX_APP_PATH)/obj

CFLAGS := -I/usr/include/GLFW/ -I/usr/include/GL/
LDFLAGS := -L$(PREFIX) -lGLEW -lX11 -lGLU -lGL -lglfw -lflutter_engine

# LDFLAGS += -L $(PREFIX) -lflutter_engine

# GLEW_CFLAGS=$(shell pkg-config --cflags glew)
# GLEW_LDFLAGS=$(shell pkg-config --libs glew)

# GLFW_CFLAGS=$(shell pkg-config --cflags glfw3)
# GLFW_LDFLAGS=$(shell pkg-config --libs glfw3)

all: $(PREFIX)/flutter_embedder

$(PREFIX)/flutter_embedder: $(PREFIX) embedder.c $(PREFIX)/libflutter_engine.so $(PREFIX)/icudtl.dat flutter_embedder.h
	$(CC) embedder.c ${CFLAGS} ${GLFW_CFLAGS} ${GLEW_CFLAGS} ${LDFLAGS} ${GLEW_LDFLAGS} ${GLFW_LDFLAGS} -o $(PREFIX)/flutter_embedder

$(PREFIX)/libflutter_engine.so flutter_embedder.h: linux-x64-embedder.zip
	unzip -o linux-x64-embedder.zip
	mv libflutter_engine.so $(PREFIX)/libflutter_engine.so

$(PREFIX)/icudtl.dat:
	cp ../icudtl.dat $(PREFIX)/icudtl.dat

linux-x64-embedder.zip:
	wget -O linux-x64-embedder.zip https://storage.googleapis.com/flutter_infra/flutter/07e2520d5d8f837da439317adab4ecd7bff2f72d/linux-x64/linux-x64-embedder

clean:
	rm -rf $(PREFIX)/libflutter_engine.so flutter_embedder.h $(PREFIX)/flutter_embedder linux-x64-embedder.zip

$(PREFIX) $(BUILD):
	mkdir -p $(PREFIX)

.PHONY: all clean
