import sys, time
sys.path.insert(0, "/home/claude/scripts")
import cadquery as cq
import esp32s3_case_v4 as C
from OCP.BRepExtrema import BRepExtrema_DistShapeShape
def vol(wp): return sum(s.Volume() for s in wp.solids().vals()) if wp.vals() else 0.0
def dist(a,b):
    d=BRepExtrema_DistShapeShape(a.val().wrapped,b.val().wrapped); d.Perform(); return d.Value()
body, door = C.make_body(), C.make_door()
print("body valid",body.val().isValid(),"solids",len(body.solids().vals()),"vol",round(vol(body)),"| door valid",door.val().isValid(),"vol",round(vol(door)))
board = cq.importers.importStep(sys.argv[1])
print("board x body %.4f | board x door %.4f"%(vol(board.intersect(body)),vol(board.intersect(door))))
print("door x body at home %.4f (ridge in dimple, taper)"%vol(door.intersect(body)))
print("min dist board<->body %.3f board<->door %.3f ridge/door %.3f"%(dist(board,body),dist(board,door),dist(body,door)))
bb=board.val().BoundingBox(); print("board z range %.2f..%.2f"%(bb.zmin,bb.zmax))
worst=0
for dy in [-v for v in range(70,0,-4)]+[-2,-1,-0.5]:
    m=door.translate((0,dy,0))
    vb=vol(board.intersect(m)); vs=vol(body.intersect(m)); worst=max(worst,vb)
    print("dy=%6.1f door x board %.4f door x body %.4f"%(dy,vb,vs))
print("worst door x board",worst)
