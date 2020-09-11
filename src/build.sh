#!/bin/sh

/home/fhunleth/nerves/nerves_system_br/o/bbb_sgx/host/usr/bin/arm-unknown-linux-gnueabihf-gcc flutter-pi.c console_keyboard.c -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64  -pipe -O2 -I/home/fhunleth/nerves/nerves_system_br/o/bbb_sgx/staging/usr/include -I/home/fhunleth/nerves/nerves_system_br/o/bbb_sgx/staging/usr/include -I/home/fhunleth/nerves/nerves_system_br/o/bbb_sgx/staging/usr/include/libdrm  --sysroot=/home/fhunleth/nerves/nerves_system_br/o/bbb_sgx/staging -L/home/fhunleth/nerves/nerves_system_br/o/bbb_sgx/staging/usr/lib -ldrm  -ldl -lgbm -lGLESv2 -lEGL -lglfw -L/home/fhunleth/smartrent/elixir_flutter_embedder/nerves_example/_build/bbb_prod/lib/flutter_embedder/priv -lflutter_engine -o /home/fhunleth/smartrent/elixir_flutter_embedder/nerves_example/_build/bbb_prod/lib/flutter_embedder/priv/flutter_embedder -pthread

sftp -b /dev/stdin nerves.local << EOF
cd /root
put /home/fhunleth/smartrent/elixir_flutter_embedder/nerves_example/_build/bbb_prod/lib/flutter_embedder/priv/flutter_embedder
EOF

echo Done.

