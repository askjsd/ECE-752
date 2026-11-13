#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
upstream="$root/.upstream"

if [[ ! -d "$upstream/.git" ]]; then
    git clone --depth 1 https://github.com/ChampSim/ChampSim.git "$upstream"
fi

git -C "$upstream" submodule update --init
"$upstream/config.sh" "$root/champsim_config.json"
make -C "$upstream" -j"${JOBS:-$(nproc)}"

echo "ChampSim built at $upstream/bin/champsim"
