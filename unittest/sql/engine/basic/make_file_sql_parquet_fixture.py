"""Small format fixtures for decoder tests, not a query engine."""
import struct
from pathlib import Path

def varint(v):
    out=bytearray()
    while v>=128:out.append((v&127)|128);v>>=7
    out.append(v)
    return bytes(out)

def integer(v):return varint((v<<1)^(v>>63))
def binary(v):
    if isinstance(v,str):v=v.encode()
    return varint(len(v))+v

def fields(values):
    out=b'';previous=0
    for key,kind,value in values:
        delta=key-previous
        out+=bytes([(delta<<4)|kind]) if 0<delta<16 else bytes([kind])+integer(key)
        out+=value;previous=key
    return out+b'\0'
def seq(kind,values):
    values=list(values);n=len(values)
    return (bytes([(n<<4)|kind]) if n<15 else bytes([240|kind])+varint(n))+b''.join(values)
def literal_snappy(data):
    n=len(data)-1
    tag=bytes([n<<2]) if n<60 else bytes([(60<<2)])+bytes([n])
    return varint(len(data))+tag+data

def make(path):
    body=b'PAR1';groups=[]
    for values in [[1,None,3],[4,5,None]]:
        levels=bytes([3,sum((v is not None)<<i for i,v in enumerate(values))])
        # Eight packed definition bits, padded to one group.
        packed=b''.join(struct.pack('<q',v) for v in values if v is not None)
        page=levels+literal_snappy(packed)
        h=fields([(1,5,integer(3)),(2,5,integer(len(levels)+len(packed))),
                  (3,5,integer(len(page))),
                  (8,12,fields([(1,5,integer(3)),(2,5,integer(1)),(3,5,integer(3)),
                                (4,5,integer(0)),(5,5,integer(len(levels))),
                                (6,5,integer(0)),(7,1,b'')]))])
        offset=len(body);body+=h+page
        metadata=fields([(1,5,integer(2)),(2,9,seq(5,[integer(0)])),
                         (3,9,seq(8,[binary('value')])),(4,5,integer(1)),
                         (5,6,integer(3)),(6,6,integer(len(h)+len(levels)+len(packed))),
                         (7,6,integer(len(h)+len(page))),(9,6,integer(offset))])
        chunk=fields([(2,6,integer(offset)),(3,12,metadata)])
        groups.append(fields([(1,9,seq(12,[chunk])),(2,6,integer(len(h)+len(page))),(3,6,integer(3))]))
    root=fields([(4,8,binary('schema')),(5,5,integer(1))])
    column=fields([(1,5,integer(2)),(3,5,integer(1)),(4,8,binary('value'))])
    footer=fields([(1,5,integer(2)),(2,9,seq(12,[root,column])),(3,6,integer(6)),(4,9,seq(12,groups))])
    Path(path).write_bytes(body+footer+struct.pack('<I',len(footer))+b'PAR1')
if __name__=='__main__':
    import sys
    make(sys.argv[1])
