#!/bin/bash
# 把 ModelBrowser 打包成自包含的 macOS .app（含 OSG 动态库与 osgDB 插件，
# 目标机器无需安装 Homebrew / OpenSceneGraph）。
# 用法：./package_app.sh [--dmg]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
DIST="$ROOT/dist"
APP="$DIST/ModelBrowser.app"
PLUGSRC="$(ls -d /opt/homebrew/lib/osgPlugins-* | head -1)"
PLUGVER="$(basename "$PLUGSRC")"

echo "==> 编译"
cmake --build "$BUILD" -j >/dev/null

echo "==> 组装 bundle 结构"
rm -rf "$DIST"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" \
         "$APP/Contents/Resources" "$APP/Contents/Resources"
cp "$BUILD/bin/ModelBrowser" "$APP/Contents/MacOS/"
cp -RL "$PLUGSRC" "$APP/Contents/Resources/$PLUGVER"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>   <string>en</string>
    <key>CFBundleExecutable</key>          <string>ModelBrowser</string>
    <key>CFBundleIdentifier</key>          <string>local.zhiyu.modelbrowser</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key>                <string>ModelBrowser</string>
    <key>CFBundleDisplayName</key>         <string>OSGB Model Browser</string>
    <key>CFBundlePackageType</key>         <string>APPL</string>
    <key>CFBundleShortVersionString</key>  <string>1.1</string>
    <key>CFBundleVersion</key>             <string>1</string>
    <key>LSMinimumSystemVersion</key>      <string>12.0</string>
    <key>NSHighResolutionCapable</key>     <true/>
    <key>NSPrincipalClass</key>            <string>NSApplication</string>
</dict>
</plist>
PLIST

echo "==> 打包依赖库并改写链接路径"
chmod -R u+w "$APP"
FRAMEWORKS="$APP/Contents/Frameworks"

# 依赖名 -> 源文件 定位：优先 OSG lib 目录，其次 Homebrew lib 目录
find_dep_source() {
    local name="$1"
    local c
    for c in "/opt/homebrew/opt/open-scene-graph/lib/$name" \
             "/opt/homebrew/lib/$name"; do
        [ -e "$c" ] && { echo "$c"; return 0; }
    done
    find /opt/homebrew/opt -maxdepth 3 -name "$name" 2>/dev/null | head -1
}

# 处理一个 Mach-O：把 /opt/homebrew 和未拷贝的 @rpath 依赖收进 Frameworks，
# 并把绝对路径引用改写为 @rpath/<name>
QUEUE=("$APP/Contents/MacOS/ModelBrowser")
for so in "$APP/Contents/Resources/$PLUGVER"/*.so; do QUEUE+=("$so"); done

i=0
while [ $i -lt ${#QUEUE[@]} ]; do
    f="${QUEUE[$i]}"; i=$((i+1))
    while IFS= read -r dep; do
        name="$(basename "$dep")"
        src=""
        case "$dep" in
            /opt/homebrew/*)
                src="$dep"
                install_name_tool -change "$dep" "@rpath/$name" "$f" 2>/dev/null
                ;;
            @rpath/*) src="";;   # 引用已是 @rpath，仅需确保库被拷贝
            *) continue;;
        esac
        if [ ! -e "$FRAMEWORKS/$name" ]; then
            [ -z "$src" ] && src="$(find_dep_source "$name")"
            if [ -n "$src" ] && [ -e "$src" ]; then
                cp -L "$src" "$FRAMEWORKS/$name"
                chmod u+w "$FRAMEWORKS/$name"
                install_name_tool -id "@rpath/$name" "$FRAMEWORKS/$name" 2>/dev/null
                QUEUE+=("$FRAMEWORKS/$name")
            fi
        fi
    done < <(otool -L "$f" | tail -n +2 | awk '{print $1}' \
             | grep -E "^/opt/homebrew|^@rpath/" || true)
done
echo "    共打包 $(ls "$FRAMEWORKS" | wc -l | tr -d ' ') 个依赖库"

# rpath：删除指向 /opt/homebrew 的残留，补上 bundle 内相对路径
fix_rpaths() {
    local f="$1" want="$2"
    otool -l "$f" | awk '/LC_RPATH/{getline;getline;print $2}' \
        | { grep "/opt/homebrew" || true; } | while IFS= read -r rp; do
            install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null || true
        done
    install_name_tool -add_rpath "$want" "$f" 2>/dev/null || true
}
fix_rpaths "$APP/Contents/MacOS/ModelBrowser" "@executable_path/../Frameworks"
for so in "$APP/Contents/Resources/$PLUGVER"/*.so; do
    fix_rpaths "$so" "@loader_path/../../Frameworks"
done
for lib in "$FRAMEWORKS"/*.dylib; do
    [ -e "$lib" ] && fix_rpaths "$lib" "@loader_path"
done

echo "==> 残留检查（bundle 内不应再引用 /opt/homebrew）"
LEFT=$(find "$APP" -type f \( -name "*.dylib" -o -name "*.so" -o -path "*/MacOS/*" \) \
        -exec otool -L {} + 2>/dev/null | grep -c "/opt/homebrew" || true)
if [ "$LEFT" -ne 0 ]; then
    echo "警告：仍有 $LEFT 处 /opt/homebrew 引用未改写"
fi

echo "==> ad-hoc 签名"
find "$APP/Contents/Frameworks" "$APP/Contents/Resources" -type f \
     \( -name "*.dylib" -o -name "*.so" \) \
     -exec codesign --force -s - {} \; 2>/dev/null
codesign --force -s - "$APP/Contents/MacOS/ModelBrowser"
codesign --force -s - "$APP"

if [ "${1:-}" = "--dmg" ]; then
    echo "==> 生成 DMG"
    hdiutil create -volname ModelBrowser -srcfolder "$APP" -ov -format UDZO \
        "$DIST/ModelBrowser.dmg" >/dev/null
fi

echo "==> 完成：$APP"
du -sh "$APP"
