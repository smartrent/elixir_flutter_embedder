ZIPFILE=linux-x64-embedder.zip
FLUTTER_ENGINE_VERSION = 9d5b21729ff53dbf8eadd8bc97e0e30d77abec95

$(PREFIX)/libflutter_engine.so: $(ZIPFILE)
	unzip -p $(ZIPFILE) libflutter_engine.so > $(PREFIX)/libflutter_engine.so

flutter_embedder.h: $(ZIPFILE)
	unzip -p $(ZIPFILE) flutter_embedder.h > flutter_embedder.h

$(PREFIX)/icudtl.dat:
	cp ../icudtl.dat $(PREFIX)/icudtl.dat

$(ZIPFILE):
	wget -q -O $(ZIPFILE) https://storage.googleapis.com/flutter_infra/flutter/$(FLUTTER_ENGINE_VERSION)/linux-x64/linux-x64-embedder