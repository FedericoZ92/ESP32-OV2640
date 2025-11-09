import re
import sys

print("cc to tflite")

if len(sys.argv) < 3:
    print("Usage: python cc_to_tflite.py <input.cc> <output.tflite>")
    sys.exit(1)

cc_path = sys.argv[1]
tflite_path = sys.argv[2]

with open(cc_path, "r") as f:
    content = f.read()

# Extract the hex bytes between braces { ... }
matches = re.findall(r'0x[0-9a-fA-F]+', content)
if not matches:
    print("Error: No hex data found in the .cc file!")
    sys.exit(1)

# Convert to bytes
data = bytes(int(x, 16) for x in matches)

with open(tflite_path, "wb") as f:
    f.write(data)

print(f"✅ Extracted {len(data)} bytes to '{tflite_path}'")
