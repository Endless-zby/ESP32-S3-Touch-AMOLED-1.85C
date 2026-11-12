#!/usr/bin/env bash
# Generate the custom LVGL fonts (Source Han Sans SC + ASCII + LVGL symbols)
# from the characters used in aicc_phone/ui_strings.h.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SK="$ROOT/aicc_phone"
FONTDIR="$ROOT/tools/fonts"
CJK_FONT="$FONTDIR/SourceHanSansSC-Normal.otf"
FA_FONT="$FONTDIR/FontAwesome5-Solid+Brands+Regular.woff"
SYMS="61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650"

if [ ! -f "$CJK_FONT" ] || [ ! -f "$FA_FONT" ]; then
  echo "Missing font sources in $FONTDIR (copy them from lvgl/scripts/built_in_font)" >&2
  exit 1
fi

CHARS="$(python3 -I - "$SK/ui_strings.h" <<'PY'
import sys
s=open(sys.argv[1],encoding='utf-8').read()
chars=sorted({c for c in s if ord(c)>127})
# always include a few extras used in dynamic text
extra='：%°'
print(''.join(chars)+extra)
PY
)"
echo "CJK chars: $CHARS"

gen() {
  local size=$1 name=$2
  npx --yes lv_font_conv@1.5.3 --bpp 4 --size "$size" --format lvgl --lv-include lvgl.h \
    --no-compress --lv-font-name "$name" \
    --font "$CJK_FONT" --symbols "$CHARS" -r 0x20-0x7E \
    --font "$FA_FONT" -r "$SYMS" \
    -o "$SK/$name.c"
  echo "generated $SK/$name.c ($(wc -c < "$SK/$name.c") bytes)"
}
gen 18 font_cn_18
gen 24 font_cn_24
