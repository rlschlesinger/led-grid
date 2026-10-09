# PlatformIO pre-build step: compress web/index.html into src/webpage.h as a
# byte array. The board serves it gzipped, about a quarter the size, which
# matters on a weak Wi-Fi link where every lost packet stalls the transfer.
Import("env")
import gzip, os

root = env["PROJECT_DIR"]
src = os.path.join(root, "web", "index.html")
dst = os.path.join(root, "src", "webpage.h")
data = gzip.compress(open(src, "rb").read(), compresslevel=9, mtime=0)
body = ",".join(str(b) for b in data)
out = (
    "// Generated from web/index.html by tools/gzip_webpage.py. Edit the HTML, not this.\n"
    "#pragma once\n#include <Arduino.h>\n\n"
    f"static const uint8_t INDEX_HTML_GZ[] PROGMEM = {{{body}}};\n"
    f"static const size_t INDEX_HTML_GZ_LEN = {len(data)};\n"
)
if not os.path.exists(dst) or open(dst).read() != out:
    open(dst, "w").write(out)
    print(f"webpage.h: {os.path.getsize(src)} B of HTML -> {len(data)} B gzipped")
