import random, sys
random.seed(3)
N=5000; D=32
out=[]
out.append("CREATE TABLE docs (id BIGINT PRIMARY KEY, cat VARCHAR(8), emb VECTOR(%d));"%D)
for b in range(0,N,100):
    vals=[]
    for i in range(b,min(b+100,N)):
        v="["+",".join("%.3f"%random.uniform(-1,1) for _ in range(D))+"]"
        vals.append("(%d,'c%d','%s')"%(i,i%5,v))
    out.append("INSERT INTO docs VALUES "+",".join(vals)+";")
print("\n".join(out))
