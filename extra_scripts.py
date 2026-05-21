"""
LumiGate pre-build script.
Converts src/pages/*.html and src/assets/*.png into C PROGMEM headers
under src/generated/ — runs at script-load time (before compilation).
"""
Import("env")
import pathlib

def escape_c(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\r", "")

def html_to_header(path: pathlib.Path, out_dir: pathlib.Path):
    content = path.read_text(encoding="utf-8")
    var = path.stem.upper().replace("-", "_") + "_HTML"
    lines = content.split("\n")
    body = "\n".join(f'    "{escape_c(l)}\\n"' for l in lines)
    h = f"// Auto-generated from {path.name} — do not edit\n"
    h += f"static const char {var}[] PROGMEM =\n{body};\n"
    out = out_dir / (path.stem + "_html.h")
    out.write_text(h, encoding="utf-8")
    print(f"  [embed] {path.name} -> {out.name}  ({len(content)} chars)")

def png_to_header(path: pathlib.Path, out_dir: pathlib.Path):
    data = path.read_bytes()
    var = path.stem.upper().replace("-", "_") + "_PNG"
    rows = ["  " + ", ".join(f"0x{b:02x}" for b in data[i:i+16]) + ","
            for i in range(0, len(data), 16)]
    h = f"// Auto-generated from {path.name} — do not edit\n"
    h += f"static const uint8_t {var}[] PROGMEM = {{\n"
    h += "\n".join(rows) + "\n};\n"
    h += f"static const size_t  {var}_LEN = {len(data)};\n"
    out = out_dir / (path.stem + "_png.h")
    out.write_text(h, encoding="utf-8")
    print(f"  [embed] {path.name} -> {out.name}  ({len(data)} bytes)")

def generate():
    root    = pathlib.Path(env.subst("$PROJECT_DIR"))
    gen_dir = root / "src" / "generated"
    gen_dir.mkdir(exist_ok=True)
    print("LumiGate: generating embedded assets...")
    for f in sorted((root / "src" / "pages").glob("*.html")):
        html_to_header(f, gen_dir)
    for f in sorted((root / "src" / "assets").glob("*.png")):
        png_to_header(f, gen_dir)
    print("LumiGate: done.")

# Run immediately so headers exist before main.cpp is compiled
generate()
