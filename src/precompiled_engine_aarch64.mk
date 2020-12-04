ZIPFILE=linux-aarch64-embedder.zip
FLUTTER_PI_VERSION=341288caed5ef3450ed545e196733fee0cf6a568
FLUTTER_PI_URL=https://github.com/ardera/flutter-pi/archive/$(FLUTTER_PI_VERSION).zip

$(PREFIX)/libflutter_engine.so: $(ZIPFILE)
	unzip -p $(ZIPFILE) flutter-pi-$(FLUTTER_PI_VERSION)/arm64/libflutter_engine.so.debug > $(PREFIX)/libflutter_engine.so

flutter_embedder.h: $(ZIPFILE)
	unzip -p $(ZIPFILE) flutter-pi-$(FLUTTER_PI_VERSION)/flutter_embedder.h > flutter_embedder.h

$(PREFIX)/icudtl.dat:
	unzip -p $(ZIPFILE) flutter-pi-$(FLUTTER_PI_VERSION)/arm64/icudtl.dat > $(PREFIX)/icudtl.dat

$(ZIPFILE):
	wget -O $(ZIPFILE) $(FLUTTER_PI_URL)