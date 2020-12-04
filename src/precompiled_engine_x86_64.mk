ZIPFILE=linux-x64-embedder.zip
FLUTTER_ENGINE_VERSION = 2c956a31c0a3d350827aee6c56bb63337c5b4e6e

$(PREFIX)/libflutter_engine.so: $(ZIPFILE)
	unzip -p $(ZIPFILE) libflutter_engine.so > $(PREFIX)/libflutter_engine.so

flutter_embedder.h: $(ZIPFILE)
	unzip -p $(ZIPFILE) flutter_embedder.h > flutter_embedder.h

$(PREFIX)/icudtl.dat:
	cp ../icudtl.dat $(PREFIX)/icudtl.dat

$(ZIPFILE):
	wget -q -O $(ZIPFILE) https://storage.googleapis.com/flutter_infra/flutter/$(FLUTTER_ENGINE_VERSION)/linux-x64/linux-x64-embedder