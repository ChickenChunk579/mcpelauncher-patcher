#!/bin/bash
set -e

echo compiling libshim
clang -fPIC -shared -nostdlib shim.c -o libshim.so
echo backing up libplayfabmultiplayer.so
cp $1/lib/x86_64/libPlayFabMultiplayer.so libPlayFabMultiplayer.so
echo adding libshim
cp libshim.so $1/lib/x86_64/libshim.so
echo patching all libs to use libshim
cd $1/lib/x86_64/
for sofile in ./*.so; do
    if [[ "$(basename "$sofile")" == "libshim.so" ]]; then
        echo "skipping $sofile"
        continue
    fi

    echo "adding libshim to $sofile"
    patchelf --add-needed "libshim.so" "$sofile"
done
echo reverting to old libPlayFabMultiplayer.so
rm libPlayFabMultiplayer.so
cp ../../../libPlayFabMultiplayer.so .
echo done!
