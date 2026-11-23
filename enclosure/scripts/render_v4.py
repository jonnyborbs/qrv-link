"""Render the printed v4 case from the STLs: python3 render_v4.py <print_files_dir> <outdir>"""
import sys, numpy as np, trimesh, vtk
from vtk.util.numpy_support import numpy_to_vtk
src, out = sys.argv[1] + "/", sys.argv[2] + "/"
Z_TOP, Z_BOT = 5.4, -2.85
def load(n): return trimesh.load(src + n, process=False)
def unflip(m):                       # print orientation -> design orientation (front = +Z)
    m = m.copy(); m.apply_transform(trimesh.transformations.rotation_matrix(np.pi, (1, 0, 0))); m.apply_translation((0, 0, Z_TOP)); return m
body = unflip(load("qrv_v4_body_print.stl")); text = unflip(load("qrv_v4_body_text_print.stl"))
door = load("qrv_v4_door_print.stl"); door.apply_translation((0, 0, Z_BOT))
print("body", body.bounds.round(2).tolist(), "door", door.bounds.round(2).tolist())

def actor(m, color, spec=0.35, power=30):
    pts = vtk.vtkPoints(); pts.SetData(numpy_to_vtk(m.vertices.astype(float)))
    cells = vtk.vtkCellArray()
    for f in m.faces: cells.InsertNextCell(3, [int(i) for i in f])
    pd = vtk.vtkPolyData(); pd.SetPoints(pts); pd.SetPolys(cells)
    n = vtk.vtkPolyDataNormals(); n.SetInputData(pd); n.SetFeatureAngle(40); n.SplittingOn(); n.Update()
    mp = vtk.vtkPolyDataMapper(); mp.SetInputConnection(n.GetOutputPort())
    a = vtk.vtkActor(); a.SetMapper(mp); p = a.GetProperty()
    p.SetColor(*color); p.SetAmbient(0.25); p.SetDiffuse(0.75); p.SetSpecular(spec); p.SetSpecularPower(power); return a

def render(actors, path, cam, focal, up, size=(1800, 1200), view_angle=None, scale=None, lights=None):
    ren = vtk.vtkRenderer(); ren.SetBackground(0.96, 0.96, 0.97); ren.SetBackground2(1, 1, 1); ren.GradientBackgroundOn()
    for a in actors: ren.AddActor(a)
    ren.RemoveAllLights()
    for pos, inten in (lights or (((-80, -120, 160), 0.9), ((120, -40, 90), 0.5), ((0, 160, 60), 0.35))):
        l = vtk.vtkLight(); l.SetPosition(*pos); l.SetFocalPoint(*focal); l.SetIntensity(inten); l.SetLightTypeToSceneLight(); ren.AddLight(l)
    rw = vtk.vtkRenderWindow(); rw.SetOffScreenRendering(1); rw.SetMultiSamples(8); rw.AddRenderer(ren); rw.SetSize(*size)
    c = ren.GetActiveCamera(); c.SetPosition(*cam); c.SetFocalPoint(*focal); c.SetViewUp(*up)
    if scale: c.ParallelProjectionOn(); c.SetParallelScale(scale)
    elif view_angle: c.SetViewAngle(view_angle)
    ren.ResetCameraClippingRange(); rw.Render()
    w = vtk.vtkWindowToImageFilter(); w.SetInput(rw); w.SetScale(1); w.Update()
    p = vtk.vtkPNGWriter(); p.SetFileName(path); p.SetInputConnection(w.GetOutputPort()); p.Write()

BLACK, ORANGE = (0.10, 0.10, 0.11), (0.98, 0.42, 0.08)
front = [actor(body, BLACK), actor(text, ORANGE, spec=0.2), actor(door, BLACK)]
ctr = (0, 30.5, 1.5)
render(front, out + "front.png", (0, 30.5, 300), ctr, (0, 1, 0), size=(1100, 1800), scale=40)
render(front, out + "iso.png", (95, -85, 95), ctr, (0, 0, 1), view_angle=22)
d2 = door.copy(); d2.apply_translation((0, -24, 0))
GREY = (0.2, 0.2, 0.22)
back = [actor(body, GREY, spec=0.6, power=25), actor(d2, (0.26, 0.26, 0.28), spec=0.6, power=25)]
below = (((-90, -80, -160), 1.0), ((100, -60, -120), 0.7), ((0, 150, -90), 0.5), ((0, -150, 40), 0.4))
render(back, out + "back.png", (70, -75, -120), (0, 24, -1), (1, 0, 0), view_angle=28, lights=below)
