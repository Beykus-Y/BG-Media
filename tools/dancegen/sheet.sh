#!/bin/sh
# usage: sheet.sh <name> <plan> <beats> <view> [extra args]
name=$1; plan=$2; beats=$3; view=$4; shift 4
./build/windows/bin/bgmedia_motion_sheet.exe --plan "$plan" --beats "$beats" --view "$view" --out "artifacts/sheets/$name.bmp" "$@" > "artifacts/sheets/$name.txt"
python -c "
from PIL import Image
Image.open('artifacts/sheets/$name.bmp').convert('RGB').save('artifacts/sheets/$name.png')
"
