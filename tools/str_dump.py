"""Quick ASCII-string extractor for binary reconnaissance."""
import sys, re

def strings(path, minlen=4):
    data = open(path, "rb").read()
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minlen, data):
        yield m.start(), m.group().decode("ascii", "replace")

if __name__ == "__main__":
    path = sys.argv[1]
    minlen = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    for off, s in strings(path, minlen):
        print(f"{off:08x}  {s}")
