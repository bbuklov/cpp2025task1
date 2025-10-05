import sys, hashlib

def sig(path):
    cnt=0; s=0; x=0
    with open(path,'rb') as f:
        for line in f:
            line=line.strip()
            if not line: continue
            u,v,w = line.split(b'\t')
            u=int(u); v=int(v); w=int(w)
            if u>v: u,v = v,u
            b = u.to_bytes(4,'little') + v.to_bytes(4,'little') + bytes([w&0xFF])
            h = int.from_bytes(hashlib.blake2b(b, digest_size=8).digest(), 'little')
            s = (s + h) & ((1<<64)-1)
            x ^= h
            cnt += 1
    return cnt, s, x

a,b = sys.argv[1], sys.argv[2]
ac,asum,ax = sig(a)
bc,bsum,bx = sig(b)
ok = (ac==bc and asum==bsum and ax==bx)
print(f"input_edges={ac}  output_edges={bc}  match={ok}")
print(f"sum64: {asum} vs {bsum}")
print(f"xor64: {ax} vs {bx}")
