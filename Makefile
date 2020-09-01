# thermostat-dev: embedder.cc libflutter_engine.so icudtl.dat flutter_embedder.h
# 	g++ embedder.cc -I/usr/include/GLFW/ -L . -lglfw -lflutter_engine -o thermostat-dev

thermostat-dev: embedder.cc libflutter_engine.so icudtl.dat flutter_embedder.h
	clang++ embedder.cc -std=c++11 -I/usr/include/GLFW/ -L . -lglfw -lflutter_engine -o thermostat-dev

run: thermostat-dev
	LD_LIBRARY_PATH=. ./thermostat-dev thermostat icudtl.dat

debug: thermostat-dev
	LD_LIBRARY_PATH=. gdb ./thermostat-dev thermostat icudtl.dat

libflutter_engine.so icudtl.dat flutter_embedder.h: linux-x64-embedder.zip
	unzip -o linux-x64-embedder.zip
	cp thermostat/build/linux/debug/bundle/data/icudtl.dat icudtl.dat

linux-x64-embedder.zip:
	wget -O linux-x64-embedder.zip https://storage.googleapis.com/flutter_infra/flutter/99c2b3a2455511c3fd37521d9b00972c5345b75b/linux-x64/linux-x64-embedder

clean:
	rm -rf libflutter_engine.so icudtl.dat flutter_embedder.h thermostat-dev