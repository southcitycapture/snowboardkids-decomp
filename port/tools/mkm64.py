#!/usr/bin/env python3
"""Compile a port input script (port/scripts/*.txt syntax) into a Mupen64 .m64,
so the same input drives the G4 port (--play) and mupen64plus (input-m64)."""
import sys, struct
BITS = {"A":0x8000,"B":0x4000,"Z":0x2000,"START":0x1000,"DU":0x0800,"DD":0x0400,
        "DL":0x0200,"DR":0x0100,"L":0x0020,"R":0x0010,"CU":0x0008,"CD":0x0004,
        "CL":0x0002,"CR":0x0001}
def buttons(s):
    if s.lower()=="all": return 0xFFFF
    b=0
    for t in s.split("+"): b |= BITS[t.upper()]
    return b
def compile_script(lines):
    cmds=[]
    for ln in lines:
        ln=ln.split("#")[0].strip()
        if not ln: continue
        p=ln.split()
        op=p[0].lower()
        if op=="wait": cmds.append(("wait",int(p[1])))
        elif op=="press": cmds.append(("press",buttons(p[1]),int(p[2]) if len(p)>2 else 1))
        elif op=="hold": cmds.append(("hold",buttons(p[1])))
        elif op=="release": cmds.append(("release",buttons(p[1])))
        elif op=="stick": cmds.append(("stick",int(p[1]),int(p[2]),int(p[3]) if len(p)>3 else -1))
        elif op=="dump": pass
        else: raise SystemExit("bad line: "+ln)
    out=bytearray(); held=0; pressed=0; left=0; sx=sy=0; sleft=-1; i=0
    while True:
        while left==0 and i<len(cmds):
            c=cmds[i]; i+=1
            if c[0]=="wait": left=c[1]
            elif c[0]=="press": pressed=c[1]; left=c[2]
            elif c[0]=="hold": held|=c[1]
            elif c[0]=="release": held&=~c[1]
            elif c[0]=="stick": sx,sy,sleft=c[1],c[2],c[3]
        if left==0 and i>=len(cmds): break
        b=held
        if left>0:
            left-=1; b|=pressed
            if left==0: pressed=0
        x,y=0,0
        if sleft!=0:
            x,y=sx,sy
            if sleft>0:
                sleft-=1
                if sleft==0: sx=sy=0
        out += bytes([(b>>8)&0xFF, b&0xFF, x&0xFF, y&0xFF])
    return bytes(out)
def write_m64(path, samples, name=b"SNOWBOARD KIDS"):
    h=bytearray(1024)
    h[0:4]=b"M64\x1a"; h[4]=3
    n=len(samples)//4
    h[0x0C:0x10]=struct.pack("<I", n)
    h[0x18:0x1C]=struct.pack("<I", n)
    h[0x15]=1; h[0x1C]=60; h[0x20]=1
    h[0xC4:0xC4+len(name)]=name
    open(path,"wb").write(bytes(h)+samples)
    print(path, n, "samples")
if __name__=="__main__":
    src, dst = sys.argv[1], sys.argv[2]
    write_m64(dst, compile_script(open(src).read().splitlines()))
