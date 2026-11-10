import random
random.seed(7)
N=20000; D=128
print("CREATE TABLE docs (id BIGINT PRIMARY KEY, cat VARCHAR(8), emb VECTOR(%d));"%D)
for b in range(0,N,100):
    vals=[]
    for i in range(b,min(b+100,N)):
        v="["+",".join("%.4f"%random.uniform(-1,1) for _ in range(D))+"]"
        vals.append("(%d,'c%d','%s')"%(i,i%10,v))
    print("INSERT INTO docs VALUES "+",".join(vals)+";")
