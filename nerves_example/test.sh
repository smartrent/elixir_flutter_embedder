#!/usr/bin/env bash
export LD_LIBRARY_PATH=_build/dev/lib/flutter_embedder/priv/
sudo --preserve-env LD_LIBRARY_PATH=_build/dev/lib/flutter_embedder/priv/ /home/connor/workspace/flutter/embedd/nerves_example/_build/dev/lib/flutter_embedder/priv/flutter_embedder /home/connor/workspace/flutter/embedd/nerves_example/_build/dev/lib/nerves_example/priv/flutter_assets icudtl.dat
