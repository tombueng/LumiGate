"""
LumiGate pre-build script.
Converts src/pages/*.html and src/assets/*.{png,css} into C PROGMEM headers
under src/generated/ — runs at script-load time (before compilation).
Also writes src/generated/version.h from the LUMIGATE_VERSION env var.
"""
Import("env")
import os
import gzip
import pathlib

def escape_c(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\r", "")

# Large static pages are gzip-compressed (served with Content-Encoding: gzip)
# to slash per-connection heap/airtime. They must contain no {{placeholders}}
# (dynamic values are fetched client-side from /info.json).
GZIP_PAGES = {"index", "config"}

def html_to_header(path: pathlib.Path, out_dir: pathlib.Path):
    var = path.stem.upper().replace("-", "_").replace(".", "_") + "_HTML"
    out = out_dir / (path.stem + "_html.h")
    if path.stem in GZIP_PAGES:
        raw = path.read_bytes()
        gz  = gzip.compress(raw, compresslevel=9)
        rows = ["  " + ", ".join(f"0x{b:02x}" for b in gz[i:i+16]) + ","
                for i in range(0, len(gz), 16)]
        h  = f"// Auto-generated from {path.name} (gzip) — do not edit\n"
        h += f"static const uint8_t {var}[] PROGMEM = {{\n"
        h += "\n".join(rows) + "\n};\n"
        h += f"static const size_t  {var}_LEN = {len(gz)};\n"
        out.write_text(h, encoding="utf-8")
        print(f"  [embed] {path.name} -> {out.name}  gzip {len(raw)} -> {len(gz)} bytes")
        return
    content = path.read_text(encoding="utf-8")
    lines = content.split("\n")
    body = "\n".join(f'    "{escape_c(l)}\\n"' for l in lines)
    h = f"// Auto-generated from {path.name} — do not edit\n"
    h += f"static const char {var}[] PROGMEM =\n{body};\n"
    out.write_text(h, encoding="utf-8")
    print(f"  [embed] {path.name} -> {out.name}  ({len(content)} chars)")

def binary_to_header(path: pathlib.Path, out_dir: pathlib.Path, type_suffix: str):
    data = path.read_bytes()
    safe = path.stem.upper().replace("-", "_").replace(".", "_")
    var  = safe + "_" + type_suffix
    rows = ["  " + ", ".join(f"0x{b:02x}" for b in data[i:i+16]) + ","
            for i in range(0, len(data), 16)]
    h  = f"// Auto-generated from {path.name} — do not edit\n"
    h += f"static const uint8_t {var}[] PROGMEM = {{\n"
    h += "\n".join(rows) + "\n};\n"
    h += f"static const size_t  {var}_LEN = {len(data)};\n"
    out_name = path.stem.replace(".", "_") + "_" + type_suffix.lower() + ".h"
    out = out_dir / out_name
    out.write_text(h, encoding="utf-8")
    print(f"  [embed] {path.name} -> {out.name}  ({len(data)} bytes)")

def generate_version(gen_dir: pathlib.Path):
    version = os.environ.get("LUMIGATE_VERSION", "dev")
    (gen_dir / "version.h").write_text(
        f'static const char FIRMWARE_VERSION[] = "{version}";\n',
        encoding="utf-8"
    )
    print(f"  [version] FIRMWARE_VERSION = {version}")

def generate():
    root    = pathlib.Path(env.subst("$PROJECT_DIR"))
    gen_dir = root / "src" / "generated"
    gen_dir.mkdir(exist_ok=True)
    print("LumiGate: generating embedded assets...")
    generate_version(gen_dir)
    for f in sorted((root / "src" / "pages").glob("*.html")):
        html_to_header(f, gen_dir)
    for f in sorted((root / "src" / "assets").glob("*.png")):
        binary_to_header(f, gen_dir, "PNG")
    for f in sorted((root / "src" / "assets").glob("*.css")):
        # Gzip-compress CSS before embedding — reduces 232 KB bootstrap to ~40 KB
        raw = f.read_bytes()
        compressed = gzip.compress(raw, compresslevel=9)
        # Write to a temp file with the original name so the header/variable
        # names stay identical (BOOTSTRAP_MIN_CSS / bootstrap_min_css.h)
        tmp = gen_dir / f.name
        tmp.write_bytes(compressed)
        binary_to_header(tmp, gen_dir, "CSS")
        tmp.unlink()
        print(f"  [gzip]  {f.name}: {len(raw)} -> {len(compressed)} bytes ({100*len(compressed)//len(raw)}%)")
    print("LumiGate: done.")

# Run immediately so headers exist before main.cpp is compiled
generate()
