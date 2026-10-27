#!/bin/bash
# Downloads the third-party sources at the versions this app was built and tested with,
# into third_party/, and applies this project's patch to UxPlay.
# Usage: ./fetch-deps.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$HERE/third_party"
mkdir -p "$DEST"

# name  repository  commit
DEPS=(
    "UxPlay   https://github.com/FDH2/UxPlay.git                28ed70a9876fe3f5864479f4e0d07abe4678383a"
    "libplist https://github.com/libimobiledevice/libplist.git  fe3dc34dc3484006e12b403f7ceb06f0ad40b6f4"
    "alac     https://github.com/macosforge/alac.git            c38887c5c5e64a4b31108733bd79ca9b2496d987"
    "openssl  https://github.com/openssl/openssl.git            c523121f902fde2929909dc7f76b13ceb4961efe"
)

for dep in "${DEPS[@]}"; do
    read -r name url commit <<< "$dep"
    dir="$DEST/$name"
    if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ]; then
        echo "$name: already at ${commit:0:7}"
        continue
    fi
    echo "$name: fetching ${commit:0:7}"
    rm -rf "$dir"
    git init -q "$dir"
    git -C "$dir" remote add origin "$url"
    git -C "$dir" fetch -q --depth 1 origin "$commit"
    git -C "$dir" -c advice.detachedHead=false checkout -q FETCH_HEAD
done

# lib/mdnsd: optionally answer every mDNS query directly, for routers that drop multicast
PATCH="$HERE/patches/uxplay-mdnsd-always-unicast-reply.patch"
if git -C "$DEST/UxPlay" apply --check "$PATCH" 2>/dev/null; then
    git -C "$DEST/UxPlay" apply "$PATCH"
    echo "UxPlay: patch applied"
else
    echo "UxPlay: patch already applied"
fi
