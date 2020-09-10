PREFIX = $(MIX_APP_PATH)/priv
BUILD  = $(MIX_APP_PATH)/obj

# glfw
# CFLAGS := -I/usr/include/GLFW/ -I/usr/include/GL/
# LDFLAGS := -L$(PREFIX) -lGLEW -lX11 -lGLU -lGL -lglfw -lflutter_engine -lpthread

# drm
CFLAGS += $(shell pkg-config libdrm --cflags)
LDFLAGS += $(shell pkg-config libdrm --libs)
LDFLAGS += -ldl -lgbm -lGLESv2 -lEGL -lglfw

LDFLAGS += -L$(PREFIX) -lflutter_engine

# GLEW_CFLAGS=$(shell pkg-config --cflags glew)
# GLEW_LDFLAGS=$(shell pkg-config --libs glew)

# GLFW_CFLAGS=$(shell pkg-config --cflags glfw3)
# GLFW_LDFLAGS=$(shell pkg-config --libs glfw3)

all: $(PREFIX)/flutter_embedder

# $(PREFIX)/flutter_embedder: $(PREFIX) embedder.c platformchannel.c erlcmd.c $(PREFIX)/libflutter_engine.so $(PREFIX)/icudtl.dat flutter_embedder.h
# 	$(CC) embedder.c platformchannel.c erlcmd.c ${CFLAGS} ${GLFW_CFLAGS} ${GLEW_CFLAGS} ${LDFLAGS} ${GLEW_LDFLAGS} ${GLFW_LDFLAGS} -o $(PREFIX)/flutter_embedder
$(PREFIX)/flutter_embedder: $(PREFIX) embedder_drm.c $(PREFIX)/libflutter_engine.so $(PREFIX)/icudtl.dat flutter_embedder.h
	$(CC) embedder_drm.c ${CFLAGS} ${LDFLAGS} -o $(PREFIX)/flutter_embedder

$(PREFIX)/libflutter_engine.so flutter_embedder.h $(PREFIX)/icudtl.dat: linux-arm-embedder.zip
	unzip -o -qq linux-arm-embedder.zip
	mv flutter-pi-e800fc3692b4d581631005faf58c65f86c882eb5/flutter_embedder.h flutter_embedder.h
	mv flutter-pi-e800fc3692b4d581631005faf58c65f86c882eb5/icudtl.dat $(PREFIX)/icudtl.dat
	mv flutter-pi-e800fc3692b4d581631005faf58c65f86c882eb5/libflutter_engine.so.debug $(PREFIX)/libflutter_engine.so
	rm -rf flutter-pi-e800fc3692b4d581631005faf58c65f86c882eb5/

linux-arm-embedder.zip:
	wget -O linux-arm-embedder.zip https://github.com/ardera/flutter-pi/archive/e800fc3692b4d581631005faf58c65f86c882eb5.zip

linux-x64-embedder.zip:
	wget -O linux-x64-embedder.zip https://storage.googleapis.com/flutter_infra/flutter/9d5b21729ff53dbf8eadd8bc97e0e30d77abec95/linux-x64/linux-x64-embedder

format:
	astyle --style=kr --indent=spaces=4 --align-pointer=name \
	    --align-reference=name --convert-tabs --attach-namespaces \
	    --max-code-length=110 --max-instatement-indent=120 --pad-header \
	    --pad-oper \
	    embedder*.c

clean:
	rm -rf $(PREFIX)/libflutter_engine.so flutter_embedder.h $(PREFIX)/flutter_embedder linux-x64-embedder.zip

$(PREFIX) $(BUILD):
	mkdir -p $(PREFIX)

.PHONY: all clean format
