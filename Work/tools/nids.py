"""Build a NID -> symbol name table from a list of known PS4 symbol names."""
import hashlib, base64, sys

SUFFIX = bytes.fromhex('518D64A635DED8C1E6B039B1C3E55230')
ALPHA = b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-'
STD = b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'
TR = bytes.maketrans(STD, ALPHA)

def nid(name):
    h = hashlib.sha1(name.encode() + SUFFIX).digest()
    v = int.from_bytes(h[:8], 'little')
    return base64.b64encode(v.to_bytes(8, 'big')).translate(TR)[:11].decode()

def table(names_file):
    t = {}
    for line in open(names_file, encoding='utf-8-sig'):
        n = line.strip()
        if n: t[nid(n)] = n
    return t

if __name__ == '__main__':
    t = table(sys.argv[1])
    with open(sys.argv[2], 'w') as f:
        for k, v in sorted(t.items()): f.write(f'{k}\t{v}\n')
    print(len(t), 'nids ->', sys.argv[2])
