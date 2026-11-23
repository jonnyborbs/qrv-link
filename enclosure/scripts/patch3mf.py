import re, sys, trimesh, zipfile
N, src, dst = sys.argv[1]+"/", sys.argv[2], sys.argv[3]
def load(n):
    m=trimesh.load(N+f"qrv_v4_{n}.stl",process=True); return m,(m.bounds[0]+m.bounds[1])/2
def block(m,c):
    v=m.vertices-c
    vs="\n".join('     <vertex x="%.9g" y="%.9g" z="%.9g"/>'%tuple(p) for p in v)
    ts="\n".join('     <triangle v1="%d" v2="%d" v3="%d"/>'%tuple(t) for t in m.faces)
    return "    <mesh>\n    <vertices>\n"+vs+"\n    </vertices>\n    <triangles>\n"+ts+"\n    </triangles>\n   </mesh>\n"
body,cb=load("body_print"); text,ct=load("body_text_print"); door,cd=load("door_print")
zin=zipfile.ZipFile(src)
s=zin.read("3D/Objects/object_1.model").decode()
parts=re.split(r'(<object id="\d+"[^>]*>\n)',s); assert len(parts)==5
tail="  </object>\n"
new1=parts[0]+parts[1]+block(body,cb)+tail+parts[3]+block(text,ct)+tail+" </resources>\n <build/>\n</model>\n"
s2=zin.read("3D/Objects/object_2.model").decode()
p2=re.split(r'(<object id="\d+"[^>]*>\n)',s2); assert len(p2)==3
new2=p2[0]+p2[1]+block(door,cd)+tail+" </resources>\n <build/>\n</model>\n"
ms=zin.read("Metadata/model_settings.config").decode()
# face counts: door object+part, body object, body part (text part unchanged)
fc=re.findall(r'face_count="(\d+)"',ms); print("old counts",fc)
old=[int(x) for x in fc]   # door obj, door part, body obj, body part, text part
new=[len(door.faces),len(door.faces),len(body.faces)+len(text.faces),len(body.faces),len(text.faces)]
it=iter(new)
ms=re.sub(r'face_count="\d+"',lambda m:'face_count="%d"'%next(it),ms)
print("new counts",re.findall(r'face_count="(\d+)"',ms))
zo=zipfile.ZipFile(dst,"w",zipfile.ZIP_DEFLATED)
for i in zin.infolist():
    d=zin.read(i.filename)
    if i.filename=="3D/Objects/object_1.model": d=new1.encode()
    elif i.filename=="3D/Objects/object_2.model": d=new2.encode()
    elif i.filename=="Metadata/model_settings.config": d=ms.encode()
    zo.writestr(i,d)
zo.close()
