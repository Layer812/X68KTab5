#!/usr/bin/env python3
"""Original YM2151 4MHz algorithm-0 test notes. No third-party song data."""
from pathlib import Path
import struct
import sys

def build(feedback):
    assert feedback in (0,5)
    header=bytearray(0x100)
    header[:4]=b'Vgm '
    struct.pack_into('<I',header,8,0x151)
    struct.pack_into('<I',header,0x30,4000000)
    struct.pack_into('<I',header,0x34,0x100-0x34)
    commands=bytearray()
    samples=0
    def reg(a,d):
        assert 0<=a<256 and 0<=d<256
        commands.extend((0x54,a,d))
    def wait(n):
        nonlocal samples
        assert 0<n<=65535
        commands.extend((0x61,n&255,n>>8))
        samples+=n
    reg(0x20,0xc0 | (feedback<<3))
    reg(0x38,0)
    reg(0x30,0x14)
    for slot,(multiple,tl) in enumerate(((1,0x15),(3,0x08),(2,0x0e),(1,0x03))):
        reg(0x40+slot*8,multiple)
        reg(0x60+slot*8,tl)
        reg(0x80+slot*8,0x1f)
        reg(0xa0+slot*8,0x01)
        reg(0xc0+slot*8,0x00)
        reg(0xe0+slot*8,0x0f)
    reg(0x08,0)
    wait(4410)
    melody=[0x38,0x3a,0x3c,0x3d,0x40,0x42,0x44,0x48,
            0x46,0x42,0x40,0x3d,0x3c,0x3a,0x38,0x34]
    for i,kc in enumerate(melody):
        reg(0x08,0)
        wait(662)
        reg(0x28,kc)
        reg(0x08,0x78)
        wait(13230 if i in (7,15) else 8820)
        reg(0x08,0)
        wait(1764)
    wait(8820)
    commands.append(0x66)
    struct.pack_into('<I',header,0x18,samples)
    result=header+commands
    struct.pack_into('<I',result,4,len(result)-4)
    return bytes(result)

def main(out):
    target=Path(out)
    target.mkdir(parents=True,exist_ok=True)
    for feedback in (0,5):
        file=target/f'YM2151_alg0_fb{feedback}_original_demo.vgm'
        file.write_bytes(build(feedback))
        print(file, len(build(feedback)))
if __name__=='__main__':
    main(sys.argv[1] if len(sys.argv)>1 else '.')
