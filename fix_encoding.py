"""One-shot script: fix mojibake in ui.py then delete itself."""
import os

path = r"c:\Users\mark\Documents\TornadoFanControl\ui.py"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

# Each pair: (bad mojibake string, correct Unicode character)
# These arise when UTF-8 multi-byte sequences are stored as individual Latin-1
# characters, then re-encoded as UTF-8.
replacements = [
    ("\u00e2\u0080\u0093", "\u2013"),   # â€" -> en dash –
    ("\u00e2\u0080\u0094", "\u2014"),   # â€" -> em dash —
    ("\u00c3\u0097",       "\u00d7"),   # Ã— -> ×
    ("\u00c2\u00b0",       "\u00b0"),   # Â° -> °
    ("\u00e2\u0086\u0092", "\u2192"),   # â†' -> →
    ("\u00e2\u0080\u009c", "\u201c"),   # â€œ -> "
    ("\u00e2\u0080\u009d", "\u201d"),   # â€ -> "
]

for bad, good in replacements:
    src = src.replace(bad, good)

with open(path, "w", encoding="utf-8") as f:
    f.write(src)

print("Encoding fixed.")
os.remove(__file__)
