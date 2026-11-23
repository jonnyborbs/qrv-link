"""
Slim two-part SLIDING case, v4: front body + back door (flipped from v3).

Body : roof (front) with engraved labels, BOOT/RESET flex tabs with plungers, NeoPixel + LED windows,
       hold-down posts, stop blocks, walls, USB-C end wall (one wide opening), dovetail channel at the BACK.
Door : plain flat dovetail plate for the back (smooth outside apart from the pry pocket), slightly tapered so it wedges tight, with two shallow
       detent dimples on its inner face (hidden) that click over ridges on the body's stop blocks.

Removal: put a flat screwdriver straight down into the pocket at the antenna end of the door and twist it a
quarter turn; it braces on the body's rear wall and pushes the door toward the USB-C end until the detents let go.

Assembly: drop the board into the open back (USB-C toward the end wall), slide the door in from the
USB-C end (+Y) until it clicks.  Print the body front-face-down, the door outer-face-down.

Run:  python3 esp32s3_case_v4.py <outdir>
"""
import sys, os, math
import cadquery as cq

# ----------------------------------------------------------------- board facts (from the STEP)
PCB_X = 14.15
PCB_Y1 = 57.0
BOARD_Y_MAX = 63.4
MODULE_HALF_W = 9.0
CONN_Y_MIN = -0.73
PAD_BOT = -1.55
TOP_MAX = 3.95
USB_CENTRES_X = {"left": -6.20, "right": 6.10}
BTN = {"BOOT": (5.23, 28.23), "RST": (5.23, 33.43)}
BTN_TOP = 3.08
NEOPIXEL = (-4.90, 17.90)
LED_SLOT = (2.4, 9.4, 18.6, 20.4)

# ----------------------------------------------------------------- design parameters
CLR = 0.25               # xy clearance around board
WALL = 2.0
FLOOR = 0.8
ROOF = 1.2
ROOF_GAP = 0.25
DOOR = 1.2               # sliding back door thickness
DOOR_GAP = 0.10          # door inner face to the lowest pad on the board underside
PRY_W, PRY_DEPTH, PRY_Z = 4.6, 2.0, 0.8   # blind screwdriver pocket in the door's rear edge: width (x), depth (y), depth into the door (z)
DETENT_X, DETENT_W, DETENT_Y = 11.0, 3.2, 61.6   # door-retention ridge (on the stop blocks) / dimple (in the door)
DETENT_H = 0.25          # ridge sticks out this far below the door's inner face (v4d: 0.15; more = stronger click)
DIMPLE_D = 0.40
REBATE = 1.2             # ledge width the lid edge sits on
LIP = 1.1                # dovetail overhang over the roof thickness (~42 deg): ~1 mm real engagement
FIT_FRONT = 0.12         # sliding clearance at the lid's front (USB-C) end ...
FIT_REAR = -0.10         # ... tapering to a 0.2 mm interference at the rear: the lid wedges tight
LID_END_CLR = 0.10
CORNER_R = 3.0
TEXT_DEPTH = 0.6
TEXT_FONT = "/usr/share/fonts/truetype/google-fonts/ofl/anton/Anton-Regular.ttf"
TEXT_GAP = 0.6            # clear gap between letters (mm) - keeps thin gaps from fusing when printed
WORD_GAP_EM = 0.30
PORT_W = 12.0            # width of PC/V+ (was 10.8); RADIO scales with it
PORT_L_EDGE, PORT_R_EDGE = -1.92, 1.70   # inner edges of RADIO / PC/V+, either side of the divider
DIV_X, PORT_Y, DIV_W, DIV_H = -0.11, 3.4, 0.8, 4.6   # divider bar between RADIO and PC/V+

PORT_LABELS = {"left": "RADIO", "right": "PC/V+"}
BTN_LABELS = {"BOOT": "BOOT", "RST": "RESET"}
# USB-C openings in the end wall: wide enough for plug overmolds, open at the top (the lid is the ceiling)
OPEN_X = {"left": (-12.4, -0.7), "right": (0.6, 12.3)}
OPEN_Z0 = -1.65
OPEN_R = 0.8

TAB_W = 3.2
TAB_SLOT = 0.45
TAB_X0, TAB_X1 = 2.6, 11.0
TAB_T = 0.7
PLUNGER_D = 2.0
PLUNGER_GAP = 0.12

POST_X = 11.5
POST_Y = (9, 21, 41, 54)
POST_Z0 = 0.15

STOP_GAP_Y = 0.12
STOP_GAP_X = 0.30
STOP_TOP = 4.0
LATCH_X = 11.0
LATCH_W = 3.2
LATCH_ROOT_Y, LATCH_END_Y = 56.5, 63.5      # 7 mm cantilever keeps flex strain ~1.5 %
LATCH_T = 0.8
LATCH_BUMP_Y = 61.6      # bump centre when the lid is fully home
LATCH_BITE = 0.4         # how far the bump bottom sits below the block top

# ----------------------------------------------------------------- derived
Z_DLEDGE = PAD_BOT - DOOR_GAP      # inner face of the door
Z_BOT = Z_DLEDGE - DOOR
Z_BLK0 = Z_DLEDGE + 0.10           # bottom of the stop blocks
Z_LEDGE = TOP_MAX + ROOF_GAP       # underside of the roof
Z_TOP = Z_LEDGE + ROOF

CX0, CX1 = -(PCB_X + CLR), PCB_X + CLR
CY0, CY1 = CONN_Y_MIN - CLR, BOARD_Y_MAX + CLR
OX0, OX1 = CX0 - WALL, CX1 + WALL
OY0, OY1 = CY0 - WALL, CY1 + WALL
RX0, RX1 = CX0 - REBATE, CX1 + REBATE
RY1 = CY1 + REBATE


def rrect(x0, x1, y0, y1, r, z0, z1):
    return (cq.Workplane("XY").workplane(offset=z0)
            .center((x0 + x1) / 2, (y0 + y1) / 2).rect(x1 - x0, y1 - y0)
            .extrude(z1 - z0).edges("|Z").fillet(r))


def box(x0, x1, y0, y1, z0, z1):
    return (cq.Workplane("XY").workplane(offset=z0)
            .center((x0 + x1) / 2, (y0 + y1) / 2).rect(x1 - x0, y1 - y0).extrude(z1 - z0))


def cyl(x, y, d, z0, z1):
    return cq.Workplane("XY").workplane(offset=z0).center(x, y).circle(d / 2).extrude(z1 - z0)


def prism_y(pts_xz, y0, y1):
    """Polygon in the XZ plane extruded along Y from y0 to y1."""
    w = cq.Workplane("XZ").polyline(pts_xz).close().extrude(y1 - y0)
    bb = w.val().BoundingBox()
    return w.translate((0, y0 - bb.ymin, 0))


def prism_x(pts_yz, x0, x1):
    """Polygon in the YZ plane extruded along X from x0 to x1."""
    w = cq.Workplane("YZ").polyline(pts_yz).close().extrude(x1 - x0)
    bb = w.val().BoundingBox()
    return w.translate((x0 - bb.xmin, 0, 0))


# ----------------------------------------------------------------- text (letter-by-letter spacing)
def _groups(txt, size, height=1.0):
    """Render txt once and return letter groups [xmin, xmax, [solids]].
    Pieces that belong to one letter are merged: i/j dots sit above their stem, and some fonts draw
    K/k as two shapes with a hairline gap (bridged here)."""
    wp = cq.Workplane("XY").text(txt, size, height, combine=False, fontPath=TEXT_FONT,
                                 halign="left", valign="center")
    items = sorted(((s.BoundingBox().xmin, s.BoundingBox().xmax, s) for s in wp.val().Solids()),
                   key=lambda t: t[0])
    groups = []
    for x0, x1, s in items:
        if groups:
            g = groups[-1]
            ov = min(g[1], x1) - max(g[0], x0)
            gap = x0 - g[1]
            if ov > 0.6 * min(g[1] - g[0], x1 - x0):
                g[0], g[1] = min(g[0], x0), max(g[1], x1)
                g[2].append(s)
                continue
            lb, sb, tol = g[2][-1].BoundingBox(), s.BoundingBox(), 0.02 * size
            if (0 <= gap < 0.012 * size and sb.ymin >= lb.ymin - tol and sb.ymax <= lb.ymax + tol):
                bridged = s.translate(cq.Vector(-(gap + 0.04 * size / 10), 0, 0))
                g[2][-1] = g[2][-1].fuse(bridged).clean()
                g[1] = max(g[1], x1)
                continue
        groups.append([x0, x1, [s]])
    chars = [c for c in txt if c != " "]
    if "0" in chars and len(chars) == len(groups):            # draw a slashed zero
        for i, c in enumerate(chars):
            if c == "0":
                groups[i] = _slashed_zero(groups[i], height)
    if "+" in chars and len(chars) == len(groups):            # draw a bold, full-height plus
        others = [g for c, g in zip(chars, groups) if c != "+"]
        if others:
            bbs = [s.BoundingBox() for g in others for s in g[2]]
            ymin, ymax = min(b.ymin for b in bbs), max(b.ymax for b in bbs)
            for i, c in enumerate(chars):
                if c == "+":
                    groups[i] = _plus(groups[i], ymin, ymax, height)
    return groups


def _plus(g, ymin, ymax, height):
    H = ymax - ymin
    W = 0.78 * H
    st = max(0.21 * H, 0.75)
    cx, cy = g[0] + W / 2, (ymin + ymax) / 2
    h = cq.Workplane("XY").center(cx, cy).rect(W, st).extrude(height)
    v = cq.Workplane("XY").center(cx, cy).rect(st, W).extrude(height)
    return [g[0], g[0] + W, [h.union(v).val()]]


def _slashed_zero(g, height):
    bbs = [s.BoundingBox() for s in g[2]]
    ymin, ymax = min(b.ymin for b in bbs), max(b.ymax for b in bbs)
    H = ymax - ymin
    W = 0.75 * H
    cx, cy = g[0] + W / 2, (ymin + ymax) / 2
    st, bw, ov = 0.17 * H, 0.11 * H, 0.08 * H
    ring = (cq.Workplane("XY").center(cx, cy).ellipse(W / 2, H / 2).extrude(height)
            .cut(cq.Workplane("XY").center(cx, cy).ellipse(W / 2 - st, H / 2 - st).extrude(height)))
    a = 0.26 * W
    p1, p2 = (cx - a, cy - H / 2 - ov), (cx + a, cy + H / 2 + ov)
    dx, dy = p2[0] - p1[0], p2[1] - p1[1]
    n = math.hypot(dx, dy)
    nx, ny = -dy / n * bw / 2, dx / n * bw / 2
    bar = (cq.Workplane("XY").polyline([(p1[0] + nx, p1[1] + ny), (p2[0] + nx, p2[1] + ny),
                                        (p2[0] - nx, p2[1] - ny), (p1[0] - nx, p1[1] - ny)])
           .close().extrude(height))
    return [g[0], g[0] + W, [ring.union(bar).val()]]


def _is_word_gap(groups, i, size):
    return groups[i + 1][0] - groups[i][1] > 0.18 * size


def fit_size(txt, max_width):
    g = _groups(txt, 10.0)
    sum_w = sum(x1 - x0 for x0, x1, _ in g) / 10.0
    nw = sum(_is_word_gap(g, i, 10.0) for i in range(len(g) - 1))
    nl = len(g) - 1 - nw
    return (max_width - TEXT_GAP * nl) / (sum_w + WORD_GAP_EM * nw)


def label_shapes(nat, size, x, y, halign):
    cursor, placed = 0.0, []
    for i, (x0, x1, sols) in enumerate(nat):
        if i > 0:
            cursor += WORD_GAP_EM * size if _is_word_gap(nat, i - 1, size) else TEXT_GAP
        for s in sols:
            placed.append(s.translate(cq.Vector(cursor - x0, 0, 0)))
        cursor += x1 - x0
    shift = {"left": 0.0, "center": -cursor / 2, "right": -cursor}[halign] + x
    return [s.translate(cq.Vector(shift, y, Z_TOP - TEXT_DEPTH)) for s in placed]


def label_plan():
    """(text, size, x, y, halign).  Sizes are fitted to the free width, then capped."""
    s_btn = min(fit_size("RESET", 14.0), 4.4)
    s_port = fit_size("PC/V+", PORT_W)          # RADIO uses the same size
    plan = []
    for name, (bx, by) in BTN.items():
        plan.append((BTN_LABELS[name], s_btn, TAB_X0 - TAB_SLOT - 0.8, by, "right"))
    # inner edges stay put (1.4 mm clear of the divider on each side); the text grows outward
    plan.append((PORT_LABELS["left"], s_port, PORT_L_EDGE, PORT_Y, "right"))
    plan.append((PORT_LABELS["right"], s_port, PORT_R_EDGE, PORT_Y, "left"))
    plan.append(("QRV Link", min(fit_size("QRV Link", 27.0), 7.0), 0.0, 52.0, "center"))
    plan.append(("by KB0RB", min(fit_size("by KB0RB", 24.0), 5.2), 0.0, 44.8, "center"))
    return plan


def text_compound(extra):
    solids = []
    for (t, sz, x, y, ha) in label_plan():
        solids += label_shapes(_groups(t, sz, TEXT_DEPTH + extra), sz, x, y, ha)
    # divider between the two port labels
    div = (cq.Workplane("XY").workplane(offset=Z_TOP - TEXT_DEPTH)
           .center(DIV_X, PORT_Y).rect(DIV_W, DIV_H).extrude(TEXT_DEPTH + extra).val())
    solids.append(div)
    return cq.Compound.makeCompound(solids)


# ----------------------------------------------------------------- dovetail channel (back of the case)
def channel_cutter():
    """Removed from the body walls below the door ledge: sloped (undercut) sides, open at the USB-C end."""
    k = LIP / DOOR
    zb = Z_BOT - 1
    hw = RX1 - k * (Z_DLEDGE - zb)
    tz = prism_y([(-RX1, Z_DLEDGE), (RX1, Z_DLEDGE), (hw, zb), (-hw, zb)], OY0 - 2, RY1)
    plan = rrect(RX0, RX1, OY0 - 2, RY1, CORNER_R - REBATE, zb, Z_DLEDGE)
    return plan.intersect(tz)


def ridge_pts(zb0):
    """YZ profile of the detent ridge that hangs from each stop block (y relative to the home bump centre)."""
    yb, zr = DETENT_Y, Z_DLEDGE - DETENT_H
    return [(yb - 2.0, zb0), (yb - 0.4, zr), (yb + 0.4, zr), (yb + 0.4 + (zb0 - zr), zb0)]   # gentle entry, 45 deg exit


def dimple_pts():
    yb, zd = DETENT_Y, Z_DLEDGE - DIMPLE_D
    return [(yb - 2.3, Z_DLEDGE + 0.01), (yb - 0.6, zd), (yb + 0.5, zd), (yb + 0.5 + DIMPLE_D + 0.01, Z_DLEDGE + 0.01)]


# ----------------------------------------------------------------- door
def make_door():
    """Plain dovetail plate for the back.  Slightly tapered: sliding clearance FIT_FRONT at the front,
    FIT_REAR (interference) at the rear, so it wedges tight as it goes home.  Wide face = inner face."""
    yl0, yl1 = OY0, RY1 - LID_END_CLR
    y0, y1 = OY0 - 7, RY1

    def hb(y):
        c = FIT_FRONT + (FIT_REAR - FIT_FRONT) * (y - yl0) / (yl1 - yl0)
        return RX1 - c

    def pts(y):
        b = hb(y)
        return [(-b, Z_DLEDGE), (b, Z_DLEDGE), (b - LIP, Z_BOT), (-(b - LIP), Z_BOT)]

    pl = cq.Plane(origin=(0, y0, 0), xDir=(1, 0, 0), normal=(0, -1, 0))   # local x = X, local y = +Z
    tz = (cq.Workplane(pl).polyline(pts(y0)).close()
          .workplane(offset=-(y1 - y0)).polyline(pts(y1)).close().loft(ruled=True))
    half = RX1 - FIT_FRONT
    plan = rrect(-half, half, OY0 - 6, yl1, CORNER_R - REBATE - FIT_FRONT, Z_BOT, Z_DLEDGE)
    outer = rrect(OX0, OX1, OY0, OY1, CORNER_R, Z_BOT, Z_DLEDGE)
    door = plan.intersect(tz).intersect(outer)
    for sx in (-1, 1):                                   # detent dimples on the inner face
        door = door.cut(prism_x(dimple_pts(), sx * DETENT_X - DETENT_W / 2 - 0.15, sx * DETENT_X + DETENT_W / 2 + 0.15))
    # pry pocket: blind, open at the rear edge, centred between the detents.  A flat screwdriver goes in
    # from the back, rests against the body's rear wall, and a quarter-twist pushes the door forward.
    # Blind (0.4 mm floor left) so the tool can never reach the board.
    yl1 = RY1 - LID_END_CLR
    pocket = (box(-PRY_W / 2, PRY_W / 2, yl1 - PRY_DEPTH, yl1 + 1.0, Z_BOT - 0.01, Z_BOT + PRY_Z)
              .edges("|Z").fillet(0.4))
    door = door.cut(pocket)
    return door


# ----------------------------------------------------------------- body (front shell)
def make_body(with_text=True):
    b = rrect(OX0, OX1, OY0, OY1, CORNER_R, Z_BOT, Z_TOP)
    b = b.cut(rrect(CX0, CX1, CY0, CY1, CORNER_R - WALL, Z_BOT - 1, Z_LEDGE))     # cavity, open at the back
    b = b.cut(channel_cutter())
    # USB-C openings: through the end wall, floor = the door, ceiling = the roof
    # one wide opening for both USB-C ports (no centre divider)
    x0, x1 = min(v[0] for v in OPEN_X.values()), max(v[1] for v in OPEN_X.values())
    b = b.cut(box(x0, x1, OY0 - 1, CY0 + 0.1, Z_DLEDGE - 0.5, Z_LEDGE).edges("|Y and >Z").fillet(OPEN_R))

    # hold-down posts hanging from the roof
    for sx in (-1, 1):
        for y in POST_Y:
            b = b.union(box(sx * POST_X - 0.6, sx * POST_X + 0.6, y - 1.5, y + 1.5, POST_Z0, Z_LEDGE + 0.01))

    # stop blocks in the antenna-end corners, hanging from the roof, with a detent ridge each
    for sx in (-1, 1):
        xa, xb = sorted((sx * (MODULE_HALF_W + STOP_GAP_X), sx * (CX1 + 0.3)))
        blk = box(xa, xb, PCB_Y1 + STOP_GAP_Y, CY1 + 0.3, Z_BLK0, Z_LEDGE + 0.01)
        blk = blk.faces("<Y").edges("<Z").chamfer(0.5)
        b = b.union(blk)
        ridge = prism_x(ridge_pts(Z_BLK0 + 0.2), sx * DETENT_X - DETENT_W / 2, sx * DETENT_X + DETENT_W / 2)
        b = b.union(ridge)

    # NeoPixel + LED windows
    b = b.cut(cyl(*NEOPIXEL, 4.4, Z_LEDGE - 1, Z_TOP + 1))
    x0, x1, y0, y1 = LED_SLOT
    b = b.cut(box(x0, x1, y0, y1, Z_LEDGE - 1, Z_TOP + 1).edges("|Z").fillet(0.6))

    # BOOT / RESET flex tabs with plungers
    for name, (bx, by) in BTN.items():
        hw = TAB_W / 2
        outer = box(TAB_X0 - TAB_SLOT, TAB_X1, by - hw - TAB_SLOT, by + hw + TAB_SLOT, Z_LEDGE - 1, Z_TOP + 1)
        inner = box(TAB_X0, TAB_X1 + 1, by - hw, by + hw, Z_LEDGE - 2, Z_TOP + 2)
        b = b.cut(outer.cut(inner))
        b = b.cut(box(TAB_X0 - 0.01, TAB_X1, by - hw - 0.01, by + hw + 0.01, Z_LEDGE - 1, Z_TOP - TAB_T))
        b = b.union(cyl(bx, by, PLUNGER_D, BTN_TOP + PLUNGER_GAP, Z_TOP - TAB_T + 0.01))

    if with_text:
        b = b.cut(cq.Workplane("XY").newObject([text_compound(0.2)]))
    return b


def make_text_cutter():
    return cq.Workplane("XY").newObject([text_compound(0.2)])


def make_body_text():
    return cq.Workplane("XY").newObject([text_compound(0.0)])


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    body, door = make_body(), make_door()
    print("outer size: %.2f x %.2f x %.2f mm" % (OX1 - OX0, OY1 - OY0, Z_TOP - Z_BOT))
    cq.exporters.export(body, os.path.join(out, "qrv_v4_body.step"))
    cq.exporters.export(door, os.path.join(out, "qrv_v4_door.step"))
    cq.exporters.export(make_body_text(), os.path.join(out, "qrv_v4_body_text.step"))

    import numpy as np, trimesh

    def tmesh(wp, tol=0.02, ang=0.15):
        v, t = wp.val().tessellate(tol, ang)
        return trimesh.Trimesh(np.array([[p.x, p.y, p.z] for p in v]), np.array(t), process=True)

    def flip(m):      # roof (front) face down on the print bed
        m = m.copy(); m.apply_transform(trimesh.transformations.rotation_matrix(np.pi, (1, 0, 0)))
        m.apply_translation((0, 0, Z_TOP)); return m

    body_solid = flip(tmesh(make_body(with_text=False)))
    cutter = flip(tmesh(make_text_cutter()))
    slab = trimesh.creation.box(extents=(200, 200, TEXT_DEPTH)); slab.apply_translation((0, 0, TEXT_DEPTH / 2))
    inlay = trimesh.boolean.intersection([cutter, slab], engine="manifold")
    body_pockets = trimesh.boolean.difference([body_solid, cutter], engine="manifold")
    body_pockets.export(os.path.join(out, "qrv_v4_body_print.stl"))
    inlay.export(os.path.join(out, "qrv_v4_body_text_print.stl"))

    dm = tmesh(door)                                      # outer (narrow) face down: already at Z_BOT
    dm.apply_translation((0, 0, -dm.bounds[0][2]))
    dm.export(os.path.join(out, "qrv_v4_door_print.stl"))
    print("body faces", len(body_pockets.faces), "text faces", len(inlay.faces), "door faces", len(dm.faces))
    print("ok")
