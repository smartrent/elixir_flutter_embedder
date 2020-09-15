#!/bin/sh

# _build/bbb_dev/lib/flutter_embedder/priv/flutter_embedder
mix compile
sftp -b /dev/stdin 192.168.1.140 << EOF
cd /root
put _build/bbb_dev/lib/flutter_embedder/priv/flutter_embedder
EOF

echo Done.