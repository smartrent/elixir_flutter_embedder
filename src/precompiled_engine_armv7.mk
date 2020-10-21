ZIPFILE=linux-arm-embedder.zip
FLUTTER_PI_VERSION=e800fc3692b4d581631005faf58c65f86c882eb5
FLUTTER_PI_URL=https://github.com/ardera/flutter-pi/archive/$(FLUTTER_PI_VERSION).zip

$(PREFIX)/libflutter_engine.so: $(ZIPFILE)
	unzip -p $(ZIPFILE) flutter-pi-$(FLUTTER_PI_VERSION)/libflutter_engine.so.debug > $(PREFIX)/libflutter_engine.so

flutter_embedder.h: $(ZIPFILE)
	unzip -p $(ZIPFILE) flutter-pi-$(FLUTTER_PI_VERSION)/flutter_embedder.h > flutter_embedder.h

$(PREFIX)/icudtl.dat:
	unzip -p $(ZIPFILE) flutter-pi-$(FLUTTER_PI_VERSION)/icudtl.dat > $(PREFIX)/icudtl.dat

$(ZIPFILE):
	wget -O $(ZIPFILE) $(FLUTTER_PI_URL)