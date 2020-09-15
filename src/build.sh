#!/bin/sh

/home/connor/workspace/flutter/o/bbb/host/usr/bin/arm-unknown-linux-gnueabihf-gcc flutter-pi.c console_keyboard.c -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64  -pipe -O2 -I/home/connor/workspace/flutter/o/bbb/staging/usr/include -I/home/connor/workspace/flutter/o/bbb/staging/usr/include -I/home/connor/workspace/flutter/o/bbb/staging/usr/include/libdrm  --sysroot=/home/connor/workspace/flutter/o/bbb/staging -L/home/connor/workspace/flutter/o/bbb/staging/usr/lib -ldrm  -ldl -lgbm -lGLESv2 -lEGL -lglfw -L/home/connor/workspace/flutter/embedd/nerves_example/_build/bbb_dev/lib/flutter_embedder/priv -lflutter_engine -o /home/connor/workspace/flutter/embedd/nerves_example/_build/bbb_dev/lib/flutter_embedder/priv/flutter_embedder -pthread

sftp -b /dev/stdin nerves.local << EOF
cd /root
put /home/connor/workspace/flutter/embedd/nerves_example/_build/bbb_dev/lib/flutter_embedder/priv/flutter_embedder
EOF

echo Done.

