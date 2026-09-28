#!/bin/bash
# Download FastPlay's dependencies for macOS: the BASS libraries (into lib/mac),
# SQLite, and the tempo libraries (into deps/). download-deps.bat is the Windows
# equivalent. The BASS headers are already in include/.
set -e

cd "$(dirname "$0")"

echo "============================================"
echo "FastPlay Dependency Downloader (macOS)"
echo "============================================"
echo

for tool in curl unzip git; do
    if ! command -v "$tool" > /dev/null; then
        echo "Error: $tool is not installed."
        exit 1
    fi
done

mkdir -p lib/mac deps
rm -rf temp_dl
mkdir -p temp_dl

# download_bass <zip url> <dylib name> <required|optional>
download_bass() {
    local url=$1
    local dylib=$2
    local need=$3
    local name
    name=$(basename "$url" .zip)

    echo "  $dylib"
    if curl -sfL "$url" -o "temp_dl/$name.zip" && unzip -qo "temp_dl/$name.zip" -d "temp_dl/$name"; then
        local found
        found=$(find "temp_dl/$name" -name "$dylib" -type f | head -n 1)
        if [ -n "$found" ]; then
            cp "$found" lib/mac/
            return 0
        fi
    fi
    if [ "$need" = "required" ]; then
        echo "Error: could not get $dylib from $url"
        exit 1
    fi
    echo "    (not available, skipped)"
}

echo "Downloading BASS libraries..."
# Linked by FastPlay
download_bass https://www.un4seen.com/files/bass24-osx.zip libbass.dylib required
download_bass https://www.un4seen.com/files/z/0/bass_fx24-osx.zip libbass_fx.dylib required
download_bass https://www.un4seen.com/files/bassmidi24-osx.zip libbassmidi.dylib required
download_bass https://www.un4seen.com/files/bassenc24-osx.zip libbassenc.dylib required
download_bass https://www.un4seen.com/files/bassenc_mp324-osx.zip libbassenc_mp3.dylib required
download_bass https://www.un4seen.com/files/bassenc_ogg24-osx.zip libbassenc_ogg.dylib required
download_bass https://www.un4seen.com/files/bassenc_flac24-osx.zip libbassenc_flac.dylib required
# Format plugins, loaded at run time
download_bass https://www.un4seen.com/files/bassflac24-osx.zip libbassflac.dylib optional
download_bass https://www.un4seen.com/files/bassopus24-osx.zip libbassopus.dylib optional
download_bass https://www.un4seen.com/files/basswv24-osx.zip libbasswv.dylib optional
download_bass https://www.un4seen.com/files/bassape24-osx.zip libbassape.dylib optional
download_bass https://www.un4seen.com/files/bassdsd24-osx.zip libbassdsd.dylib optional
download_bass https://www.un4seen.com/files/basshls24-osx.zip libbasshls.dylib optional
download_bass https://www.un4seen.com/files/bassmix24-osx.zip libbassmix.dylib optional

echo
echo "Downloading SQLite..."
curl -sfL https://sqlite.org/2026/sqlite-amalgamation-3510200.zip -o temp_dl/sqlite.zip
unzip -qo temp_dl/sqlite.zip -d temp_dl/sqlite
cp temp_dl/sqlite/sqlite-amalgamation-3510200/sqlite3.c src/

echo
echo "Downloading tempo libraries..."
clone() {
    rm -rf "$2"
    git clone --quiet --depth 1 "$1" "$2"
}
clone https://github.com/google/speedy.git deps/speedy
clone https://github.com/Signalsmith-Audio/signalsmith-stretch.git deps/signalsmith-stretch
clone https://github.com/Signalsmith-Audio/linear.git deps/signalsmith-stretch/signalsmith-linear
clone https://github.com/waywardgeek/sonic.git deps/sonic
clone https://github.com/mborgerding/kissfft.git deps/kissfft

echo
echo "Downloading FDK AAC (for xHE-AAC)..."
clone https://github.com/mstorsjo/fdk-aac.git deps/fdk-aac

rm -rf temp_dl

echo
echo "Done. Build with:"
echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build build --parallel \"$(sysctl -n hw.ncpu)\""
