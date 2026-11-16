#!/usr/bin/env python3
"""Parametric model of the controller cabinet (CadQuery).  DRAFT 1.

Run:  python3 cabinet.py      -> writes STEP / STL / preview SVG into ./out

Axes: x = width (left to right seen from the front), y = depth (front to
rear), z = up.  Rear wall is at y = D.  All sizes in mm.

Cut-out sizes marked VERIFY are placeholders until the real part is on the
table; change the constant and re-run.
"""
import math
import os
import cadquery as cq
from cadquery import exporters

# ------------------------------------------------------------------ box
W, D = 270.0, 190.0          # footprint
HF, HR = 80.0, 115.0         # outer height at the front / at the rear
T, FL, LID_T = 3.0, 3.0, 3.0  # wall, floor, lid thickness
A = math.atan((HR - HF) / D)  # lid slope (about 10.4 deg)
A_DEG = math.degrees(A)

# ------------------------------------------------------------------ parts
PSU_L, PSU_W, PSU_H = 129.0, 97.0, 30.0     # Mean Well LRS-100-24
PSU_X0, PSU_Y0, PSU_GAP = 4.5, 13.0, 10.0   # stack position, gap between the two
PCB_W, PCB_D = 118.7, 93.0
PCB_X0, PCB_Y0, PCB_STANDOFF = 128.0, 20.0, 8.0
# PCB holes from the outline's top-left corner, KiCad orientation (y down)
PCB_HOLES = [(6.0, 5.0), (113.7, 5.0), (24.5, 88.5), (94.7, 88.5)]

INSERT_D, INSERT_DEPTH = 4.0, 7.0           # M3 heat-set insert pocket
SCREW_D = 3.4

# ------------------------------------------------------------------ rear panel
M16_HOLE_D = 16.2            # VERIFY against the socket datasheet
M16_Z, M16_X0, M16_PITCH = 42.0, 95.0, 27.0
M16_LABELS = ["EYE", "ENC", "GUN 4", "GUN 3", "GUN 2", "GUN 1"]   # in +x order
IEC_W, IEC_H = 27.5, 47.5    # VERIFY: snap-in inlet with switch and fuse
IEC_X, IEC_Z = 40.0, 38.0
USB_X, USB_Z, USB_HOLE_D = 40.0, 90.0, 24.0   # "D" type USB feed-through

BOSS = 10.0
LID_BOSSES = [(BOSS / 2 + 1, BOSS / 2 + 1), (W - BOSS / 2 - 1, BOSS / 2 + 1),
              (BOSS / 2 + 1, D - BOSS / 2 - 1), (W - BOSS / 2 - 1, D - BOSS / 2 - 1),
              (W / 2, BOSS / 2 + 1), (70.0, D - BOSS / 2 - 1)]   # rear one sits between USB and EYE
POST_X, POST_Y, POST_SX, POST_SY = 107.0, 70.0, 10.0, 12.0   # PSU strap post
STACK_TOP = FL + 2 * PSU_H + PSU_GAP                          # 73
STRAP_T = 4.0


def zt(y):
    """Outer height of the body (underside of the lid) at depth y."""
    return HF + (HR - HF) * y / D


def box(x0, x1, y0, y1, z0, z1):
    return cq.Workplane("XY").box(x1 - x0, y1 - y0, z1 - z0, centered=False).translate((x0, y0, z0))


def zcyl(x, y, d, z0, z1):
    return cq.Workplane("XY").circle(d / 2).extrude(z1 - z0).translate((x, y, z0))


def ycyl(x, z, d, y0, y1):
    c = cq.Workplane("XY").circle(d / 2).extrude(y1 - y0)
    return c.rotate((0, 0, 0), (1, 0, 0), -90).translate((x, y0, z))


def lid_normal_cyl(x, y, d, below, above):
    """Cylinder along the lid normal through the lid plane point above (x, y)."""
    c = cq.Workplane("XY").circle(d / 2).extrude(below + above).translate((0, 0, -below))
    return c.rotate((0, 0, 0), (1, 0, 0), A_DEG).translate((x, y, zt(y)))


def profile(pts, x0, x1):
    return cq.Workplane("YZ").polyline(pts).close().extrude(x1 - x0).translate((x0, 0, 0))


def make_body():
    outer = profile([(0, 0), (D, 0), (D, HR), (0, HF)], 0, W)
    cavity = profile([(T, FL), (D - T, FL), (D - T, HR + 20), (T, HR + 20)], T, W - T)
    body = outer.cut(cavity)

    # --- columns that reach the lid (trimmed by the slope)
    cols = None
    for x, y in LID_BOSSES:
        c = box(x - BOSS / 2 - 1, x + BOSS / 2 + 1, y - BOSS / 2 - 1, y + BOSS / 2 + 1, 0, HR + 5)
        cols = c if cols is None else cols.union(c)
    body = body.union(cols.intersect(outer))

    # --- PSU strap post, locating ribs
    body = body.union(box(POST_X - POST_SX / 2, POST_X + POST_SX / 2,
                          POST_Y - POST_SY / 2, POST_Y + POST_SY / 2, 0, STACK_TOP + 0.5))
    px1, py1 = PSU_X0 + PSU_W, PSU_Y0 + PSU_L
    for y0 in (15, 105):
        body = body.union(box(px1 + 0.5, px1 + 2.5, y0, y0 + 20, 0, FL + 8))
    body = body.union(box(30, 80, T, PSU_Y0 - 0.5, 0, FL + 6))
    for x0 in (10, 85):
        body = body.union(box(x0, x0 + 10, py1 + 0.5, py1 + 3, 0, FL + 6))

    # --- PCB standoffs
    for hx, hy in PCB_HOLES:
        x, y = PCB_X0 + hx, PCB_Y0 + (PCB_D - hy)
        body = body.union(zcyl(x, y, 8.0, 0, FL + PCB_STANDOFF))
        body = body.cut(zcyl(x, y, INSERT_D, FL + PCB_STANDOFF - INSERT_DEPTH, FL + PCB_STANDOFF + 1))

    # --- insert pockets: lid bosses (along the lid normal) and strap post
    for x, y in LID_BOSSES:
        body = body.cut(lid_normal_cyl(x, y, INSERT_D, 9, 1))
    body = body.cut(zcyl(POST_X, POST_Y, INSERT_D, STACK_TOP + 0.5 - INSERT_DEPTH, STACK_TOP + 2))
    # slot in the left wall for the strap's tab
    body = body.cut(box(-1, T + 1, POST_Y - 5.5, POST_Y + 5.5, STACK_TOP + 0.3, STACK_TOP + 0.9 + STRAP_T))

    # --- rear wall
    for i in range(6):
        body = body.cut(ycyl(M16_X0 + i * M16_PITCH, M16_Z, M16_HOLE_D, D - T - 1, D + 1))
    body = body.cut(box(IEC_X - IEC_W / 2, IEC_X + IEC_W / 2, D - T - 1, D + 1,
                        IEC_Z - IEC_H / 2, IEC_Z + IEC_H / 2))
    body = body.cut(ycyl(USB_X, USB_Z, USB_HOLE_D, D - T - 1, D + 1))
    for sx, sz in ((-9.5, 12.0), (9.5, -12.0)):
        body = body.cut(ycyl(USB_X + sx, USB_Z + sz, 3.2, D - T - 1, D + 1))
    labels = [(M16_X0 + i * M16_PITCH, M16_Z + 17, s, 5.5) for i, s in enumerate(M16_LABELS)]
    labels += [(USB_X, USB_Z - 22, "USB", 5.5)]
    for x, z, s, size in labels:
        plane = cq.Plane(origin=(x, D, z), xDir=(-1, 0, 0), normal=(0, 1, 0))
        body = body.cut(cq.Workplane(plane).text(s, size, -0.6, combine=False))

    # --- ventilation slots in both side walls: low row and high row near the rear
    for x0 in (-1, W - T - 1):
        for i in range(13):
            y = 20 + i * 9
            body = body.cut(box(x0, x0 + T + 2, y, y + 3, 15, 45))
        for i in range(7):
            y = 118 + i * 9
            body = body.cut(box(x0, x0 + T + 2, y, y + 3, 62, 90))
    return body


def make_lid():
    h = LID_T / math.cos(A)
    lid = profile([(0, HF), (D, HR), (D, HR + h), (0, HF + h)], 0, W)
    for x, y in LID_BOSSES:
        lid = lid.cut(lid_normal_cyl(x, y, SCREW_D, 1, LID_T + 2))
    return lid


def make_strap():
    s = box(0.5, POST_X + POST_SX / 2, POST_Y - 5, POST_Y + 5, STACK_TOP + 0.5, STACK_TOP + 0.5 + STRAP_T)
    return s.cut(zcyl(POST_X, POST_Y, SCREW_D, STACK_TOP, STACK_TOP + 10))


def make_spacer():
    z0 = FL + PSU_H
    x0, y0 = PSU_X0, PSU_Y0
    ring = box(x0, x0 + PSU_W, y0, y0 + PSU_L, z0, z0 + PSU_GAP)
    return ring.cut(box(x0 + 8, x0 + PSU_W - 8, y0 + 8, y0 + PSU_L - 8, z0 - 1, z0 + PSU_GAP + 1))


def dummies():
    """Stand-ins for the purchased parts, for the preview only."""
    out = []
    for k in range(2):
        z0 = FL + k * (PSU_H + PSU_GAP)
        out.append(box(PSU_X0, PSU_X0 + PSU_W, PSU_Y0, PSU_Y0 + PSU_L, z0, z0 + PSU_H))
    z = FL + PCB_STANDOFF
    out.append(box(PCB_X0, PCB_X0 + PCB_W, PCB_Y0, PCB_Y0 + PCB_D, z, z + 1.6))
    out.append(box(PCB_X0 + 62, PCB_X0 + 92, PCB_Y0 + 20, PCB_Y0 + 88, z + 1.6, z + 22))   # DevKit
    for x0 in (PCB_X0 + 2, PCB_X0 + PCB_W - 10):                                            # terminals
        out.append(box(x0, x0 + 8, PCB_Y0 + 5, PCB_Y0 + 80, z + 1.6, z + 16))
    return out


def flat(lid):
    """Lid laid flat for printing."""
    return lid.translate((0, 0, -HF)).rotate((0, 0, 0), (1, 0, 0), -A_DEG)


def compound(items):
    return cq.Compound.makeCompound([i.val() for i in items])


if __name__ == "__main__":
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
    os.makedirs(out, exist_ok=True)
    body, lid, strap, spacer = make_body(), make_lid(), make_strap(), make_spacer()
    parts = {"body": body, "lid": flat(lid), "psu_strap": strap, "psu_spacer": spacer}
    for name, p in parts.items():
        exporters.export(p, os.path.join(out, f"cabinet_{name}.step"))
        exporters.export(p, os.path.join(out, f"cabinet_{name}.stl"), tolerance=0.05, angularTolerance=0.2)
    opt = {"width": 1100, "height": 760, "showAxes": False, "showHidden": False,
           "strokeWidth": 0.5, "marginLeft": 30, "marginTop": 30, "projectionDir": (0, 0, 1)}

    def view(items, name, azimuth, elevation):
        """Line drawing seen from the given azimuth (0 = from the front) and elevation."""
        c = cq.Workplane("XY").add(compound(items)).translate((-W / 2, -D / 2, 0))
        c = c.rotate((0, 0, 0), (0, 0, 1), azimuth).rotate((0, 0, 0), (1, 0, 0), elevation - 90)
        exporters.export(c, os.path.join(out, f"preview_{name}.svg"), opt=opt)   # not committed

    view([body, strap, spacer] + dummies(), "open", 25, 50)
    view([body, lid], "rear", 205, 25)
    view([body, lid], "front", -30, 30)
    bb = body.val().BoundingBox()
    print("body bbox", round(bb.xlen, 1), round(bb.ylen, 1), round(bb.zlen, 1),
          "volume cm3", round(body.val().Volume() / 1000), "lid slope deg", round(A_DEG, 1))
