#!/bin/sh

# _build/rpi3_dev/lib/flutter_embedder/priv/flutter_embedder
mix compile
sftp -b /dev/stdin 192.168.1.127 << EOF
cd /root
put _build/rpi3_dev/lib/flutter_embedder/priv/flutter_embedder
EOF

echo Done.