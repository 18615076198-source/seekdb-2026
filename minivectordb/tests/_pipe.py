import os, random, subprocess, sys
N=int(os.environ.get("N","3000")); D=128
random.seed(5)
env=dict(os.environ); env["MVDB_DATA_DIR"]="/tmp/piped"
env["MVDB_MEM_BUDGET_MB"]=os.environ.get("BUDGET","16")
p=subprocess.Popen(["./build/mvdb"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=None, text=True, encoding="utf-8", env=env)
p.stdin.write("CREATE TABLE docs (id BIGINT PRIMARY KEY, cat VARCHAR(8), emb VECTOR(%d));\n"%D); p.stdin.flush(); p.stdout.readline()
for b in range(0,N,100):
    vals=[]
    for i in range(b,min(b+100,N)):
        v="["+",".join("%.4f"%random.uniform(-1,1) for _ in range(D))+"]"
        vals.append("(%d,'c%d','%s')"%(i,i%10,v))
    p.stdin.write("INSERT INTO docs VALUES "+",".join(vals)+";\n"); p.stdin.flush()
    p.stdout.readline()
p.stdin.close(); p.wait()
print("done")
