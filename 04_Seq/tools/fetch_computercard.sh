#!/usr/bin/env bash
# Fetch the latest ComputerCard.h header into this card's source directory.
#
# ComputerCard is a header-only C++ library by Chris Johnson (MIT licensed)
# that handles all hardware aspects of the Workshop Computer. We vendor it
# rather than git-submodule it so this card builds standalone and so the
# version of the API we depend on is captured in our git history.
#
# Re-run this script if you want to bump to a newer ComputerCard release.
# Always test thoroughly after a bump — check the release notes for any
# behavioural changes, especially around timing-critical paths.
set -euo pipefail

URL="https://raw.githubusercontent.com/TomWhitwell/Workshop_Computer/main/Demonstrations+HelloWorlds/PicoSDK/ComputerCard/ComputerCard.h"
DEST="$(cd "$(dirname "$0")/.." && pwd)/ComputerCard.h"

echo "Fetching ComputerCard.h → $DEST"
curl -fsSL "$URL" -o "$DEST"

# Quick sanity check: print the version line so we capture it in commit msgs.
grep -m1 '^version ' "$DEST" || true
echo "Done. Remember to commit ComputerCard.h with a message that names the version."