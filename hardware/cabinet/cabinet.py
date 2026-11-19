#!/usr/bin/env python3
"""Parametric model of the controller cabinet (CadQuery).  DRAFT 2.

Run:  python3 cabinet.py      -> writes STEP / STL / preview SVG into ./out

Table-top box for a standing operator.  The touch screen sits in a separate
front panel that leans back 15 degrees from vertical; the top is a flat
removable lid; all cables leave through the rear wall.

Axes: x = width (left to right seen from the front), y = depth (front to
rear), z = up.  The front panel's outer face passes through the bottom front
edge (y = 0, z = 0).  All sizes in mm.

Constants marked VERIFY are taken from datasheets or the maker's 3D model but
not yet checked on the real part.
"""
import math
import os
import cadquery as cq
from cadquery import exporters

# ------------------------------------------------------------------ box
W, D = 270.0, 250.0
T, FL, LID_T, PT = 3.0, 3.0, 3.0, 3.0      # wall, floor, top lid, front panel
TILT = 15.0                                 # front panel leans back from vertical
A = math.radians(TILT)
PANEL_L = 183.0                             # front panel length along the slope
H = PANEL_L * math.cos(A) - LID_T           # body height (top of the walls), about 174
SETBACK = PANEL_L * math.sin(A)             # top front edge is this far behind the bottom one

# ------------------------------------------------------------------ screen (Elecrow CrowPanel Advanced 10.1)
# From the maker's STEP model (3D file/ESP32-P4-10_1-inch-20251230.stp)
GLASS_W, GLASS_H = 235.0, 143.0             # front glass / LCD outline
SCR_PCB_W, SCR_PCB_H = 247.0, 147.0
SCR_HOLE_DX, SCR_HOLE_DY = 241.0, 141.0     # 4 x M3 (3.2) holes, VERIFY on the real board
GLASS_TO_PCB = 6.8                          # glass front face to PCB front face
SCR_DEPTH = 16.6                            # glass front face to the deepest part (rear USB-C)
WINDOW_LIP = 1.5                            # panel overlaps the glass edge by this much
WINDOW_W, WINDOW_H = GLASS_W - 2 * WINDOW_LIP, GLASS_H - 2 * WINDOW_LIP
SCR_STANDOFF_D, SCR_PILOT_D = 5.4, 2.6      # self-tapping M3 into the standoff
SCR_V = PANEL_L / 2                         # screen centre, measured up the slope

# ------------------------------------------------------------------ internal parts
SHIFT = 30.0                                # everything inside moved back to clear the screen
PSU_L, PSU_W, PSU_H = 129.0, 97.0, 30.0     # Mean Well LRS-100-24
PSU_X0, PSU_Y0, PSU_GAP = 4.5, 13.0 + SHIFT, 10.0
PCB_W, PCB_D = 118.7, 93.0
PCB_X0, PCB_Y0, PCB_STANDOFF = 128.0, 20.0 + SHIFT, 8.0
PCB_HOLES = [(6.0, 5.0), (113.7, 5.0), (24.5, 88.5), (94.7, 88.5)]   # KiCad, y down

INSERT_D, INSERT_DEPTH = 4.0, 7.0           # M3 heat-set insert pocket, VERIFY with the inserts
SCREW_D = 3.4

POST_X, POST_Y, POST_SX, POST_SY = 107.0, 70.0 + SHIFT, 10.0, 12.0   # PSU strap post
STACK_TOP = FL + 2 * PSU_H + PSU_GAP
STRAP_T = 4.0

# 35 mm DIN rail: fuse terminals, 0 V / +24 V / PE terminals, R1, DDR-30G-5 (35 mm)
RAIL_X0, RAIL_LEN, RAIL_Y = 104.0, 155.0, 150.0 + SHIFT
RAIL_BOSS_X = (RAIL_X0 + 10.0, RAIL_X0 + 135.0)   # 125 mm apart: rail slots are on a 25 mm pitch
RAIL_BOSS_H = 5.0
TERMINAL_LEN, TERMINAL_H = 60.0, 60.0       # envelope of the parts on the rail

# ------------------------------------------------------------------ rear wall
M16_HOLE_D = 18.05           # Amphenol C091A front mount: 18 +0.1, key slot 2.8 to 19.1, panel 1-5 mm
M16_KEY_W, M16_KEY_DEPTH = 2.85, 19.15 - 18.05      # slot reaches 19.1 below the top of the hole
M16_Z, M16_X0, M16_PITCH = 42.0, 95.0, 27.0
M16_LABELS = ["EYE", "ENC", "GUN 4", "GUN 3", "GUN 2", "GUN 1"]   # in +x order
IEC_W, IEC_H = 34.4, 70.1    # Bulgin BZV01/Z0000/10, vertical, cut-out minimum; panel thickness VERIFY
IEC_X, IEC_Z = 40.0, 52.0
USB_X, USB_Z, USB_HOLE_D = 40.0, 117.0, 24.0   # Neutrik NAUSB-W, D type, screws 19 x 24

BOSS = 10.0
LID_POSTS = [(BOSS / 2 + 1, D - BOSS / 2 - 1), (W - BOSS / 2 - 1, D - BOSS / 2 - 1),
             (70.0, D - BOSS / 2 - 1)]   # middle post between the mains inlet and the EYE socket
LID_FRONT_Y = SETBACK + 14.0                  # front lid bosses hang from the side walls
LID_BRACKETS = [(BOSS / 2 + 1, LID_FRONT_Y), (W - BOSS / 2 - 1, LID_FRONT_Y)]
PANEL_SCREWS = [(8.0, 8.0), (W - 8.0, 8.0), (W / 2, 8.0),
                (8.0, PANEL_L - 8.0), (W - 8.0, PANEL_L - 8.0)]     # (x, up the slope)


# ------------------------------------------------------------------ helpers
def box(x0, x1, y0, y1, z0, z1):
    return cq.Workplane("XY").box(x1 - x0, y1 - y0, z1 - z0, centered=False).translate((x0, y0, z0))


def zcyl(x, y, d, z0, z1):
    return cq.Workplane("XY").circle(d / 2).extrude(z1 - z0).translate((x, y, z0))


def ycyl(x, z, d, y0, y1):
    c = cq.Workplane("XY").circle(d / 2).extrude(y1 - y0)
    return c.rotate((0, 0, 0), (1, 0, 0), -90).translate((x, y0, z))


def profile(pts, x0, x1):
    return cq.Workplane("YZ").polyline(pts).close().extrude(x1 - x0).translate((x0, 0, 0))


N_OUT = cq.Vector(0, -math.cos(A), math.sin(A))     # front panel outward normal
UP = cq.Vector(0, math.sin(A), math.cos(A))         # up the slope


def pplane(depth=0.0):
    """Plane of the front panel; depth > 0 moves it inward from the outer face."""
    return cq.Plane(origin=N_OUT * -depth, xDir=(1, 0, 0), normal=N_OUT.toTuple())


def pt(u, v, depth=0.0):
    """World point at (x = u, v up the slope, depth inward from the outer face)."""
    p = UP * v + N_OUT * -depth
    return cq.Vector(u, p.y, p.z)


def pbox(u0, u1, v0, v1, d0, d1):
    """Box in panel coordinates, from depth d0 to d1 (inward)."""
    return (cq.Workplane(pplane(d0)).center((u0 + u1) / 2, (v0 + v1) / 2)
            .rect(u1 - u0, v1 - v0).extrude(-(d1 - d0)))


def pcyl(u, v, dia, d0, d1):
    return cq.Workplane(pplane(d0)).center(u, v).circle(dia / 2).extrude(-(d1 - d0))


def behind_panel():
    """Everything behind the panel's inner face."""
    return pbox(-50, W + 50, -100, PANEL_L + 200, PT, PT + 600)


# ------------------------------------------------------------------ parts
def make_body():
    outer = profile([(-50, 0), (D, 0), (D, H), (-50, H)], 0, W).intersect(behind_panel())
    cavity = profile([(-60, FL), (D - T, FL), (D - T, H + 20), (-60, H + 20)], T, W - T)
    body = outer.cut(cavity)

    # --- rear lid posts (full height) and front lid brackets (hanging from the side walls)
    for x, y in LID_POSTS:
        body = body.union(box(x - BOSS / 2 - 1, x + BOSS / 2 + 1, y - BOSS / 2 - 1, D, 0, H))
        body = body.cut(zcyl(x, y, INSERT_D, H - INSERT_DEPTH, H + 1))
    for x, y in LID_BRACKETS:
        x0 = 0 if x < W / 2 else W - BOSS - 2
        body = body.union(box(x0, x0 + BOSS + 2, y - BOSS / 2, y + BOSS / 2, H - 14, H))
        body = body.cut(zcyl(x, y, INSERT_D, H - INSERT_DEPTH, H + 1))

    # --- front panel bosses (behind the panel's inner face) with inserts along the panel normal
    for u, v in PANEL_SCREWS:
        b = pbox(u - BOSS / 2, u + BOSS / 2, v - BOSS / 2, v + BOSS / 2, PT, PT + 10)
        if u < W / 2 - 1:
            b = b.union(pbox(0, u, v - BOSS / 2, v + BOSS / 2, PT, PT + 10))
        elif u > W / 2 + 1:
            b = b.union(pbox(u, W, v - BOSS / 2, v + BOSS / 2, PT, PT + 10))
        body = body.union(b.intersect(outer))
        body = body.cut(pcyl(u, v, INSERT_D, PT - 0.5, PT + INSERT_DEPTH))

    # --- PSU strap post, locating ribs
    body = body.union(box(POST_X - POST_SX / 2, POST_X + POST_SX / 2,
                          POST_Y - POST_SY / 2, POST_Y + POST_SY / 2, 0, STACK_TOP + 0.5))
    px1, py1 = PSU_X0 + PSU_W, PSU_Y0 + PSU_L
    for y0 in (PSU_Y0 + 2, PSU_Y0 + 92):
        body = body.union(box(px1 + 0.5, px1 + 2.5, y0, y0 + 20, 0, FL + 8))
    for x0 in (10, 85):
        body = body.union(box(x0, x0 + 10, PSU_Y0 - 3, PSU_Y0 - 0.5, 0, FL + 6))
        body = body.union(box(x0, x0 + 10, py1 + 0.5, py1 + 3, 0, FL + 6))

    # --- controller PCB standoffs
    for hx, hy in PCB_HOLES:
        x, y = PCB_X0 + hx, PCB_Y0 + (PCB_D - hy)
        body = body.union(zcyl(x, y, 8.0, 0, FL + PCB_STANDOFF))
        body = body.cut(zcyl(x, y, INSERT_D, FL + PCB_STANDOFF - INSERT_DEPTH, FL + PCB_STANDOFF + 1))

    # --- DIN rail bosses
    for x in RAIL_BOSS_X:
        body = body.union(zcyl(x, RAIL_Y, 10.0, 0, FL + RAIL_BOSS_H))
        body = body.cut(zcyl(x, RAIL_Y, INSERT_D, FL + RAIL_BOSS_H - INSERT_DEPTH, FL + RAIL_BOSS_H + 1))

    # --- strap post insert, slot in the left wall for the strap's tab
    body = body.cut(zcyl(POST_X, POST_Y, INSERT_D, STACK_TOP + 0.5 - INSERT_DEPTH, STACK_TOP + 2))
    body = body.cut(box(-1, T + 1, POST_Y - 5.5, POST_Y + 5.5, STACK_TOP + 0.3, STACK_TOP + 0.9 + STRAP_T))

    # --- rear wall
    for i in range(6):
        x = M16_X0 + i * M16_PITCH
        body = body.cut(ycyl(x, M16_Z, M16_HOLE_D, D - T - 1, D + 1))
        # key slot pointing down: pin 2 (180 deg / 3-pole) or pin 3 (240 deg) ends up at the top
        body = body.cut(box(x - M16_KEY_W / 2, x + M16_KEY_W / 2, D - T - 1, D + 1,
                            M16_Z - (M16_HOLE_D / 2 + M16_KEY_DEPTH), M16_Z))
    body = body.cut(box(IEC_X - IEC_W / 2, IEC_X + IEC_W / 2, D - T - 1, D + 1,
                        IEC_Z - IEC_H / 2, IEC_Z + IEC_H / 2))
    body = body.cut(ycyl(USB_X, USB_Z, USB_HOLE_D, D - T - 1, D + 1))
    for sx, sz in ((-9.5, 12.0), (9.5, -12.0)):
        body = body.cut(ycyl(USB_X + sx, USB_Z + sz, 3.2, D - T - 1, D + 1))
    labels = [(M16_X0 + i * M16_PITCH, M16_Z + 17, s, 5.5) for i, s in enumerate(M16_LABELS)]
    labels += [(USB_X, USB_Z + 20, "USB", 5.5)]
    for x, z, s, size in labels:
        plane = cq.Plane(origin=(x, D, z), xDir=(-1, 0, 0), normal=(0, 1, 0))
        body = body.cut(cq.Workplane(plane).text(s, size, -0.6, combine=False))

    # --- ventilation slots in both side walls: low row and high row near the rear
    for x0 in (-1, W - T - 1):
        for i in range(13):
            y = PSU_Y0 + 7 + i * 9
            body = body.cut(box(x0, x0 + T + 2, y, y + 3, 15, 45))
        for i in range(8):
            y = 165 + i * 9
            body = body.cut(box(x0, x0 + T + 2, y, y + 3, 100, 150))
    return body


def make_lid():
    lid = box(0, W, 0, D, H, H + LID_T).intersect(behind_panel())
    for x, y in LID_POSTS + LID_BRACKETS:
        lid = lid.cut(zcyl(x, y, SCREW_D, H - 1, H + LID_T + 1))
    return lid


def make_panel():
    panel = pbox(0, W, 0, PANEL_L, 0, PT)
    # screen window; the glass sits against the panel's inner face behind the lip
    panel = panel.cut(pbox(W / 2 - WINDOW_W / 2, W / 2 + WINDOW_W / 2,
                           SCR_V - WINDOW_H / 2, SCR_V + WINDOW_H / 2, -1, PT + 1))
    # standoffs for the screen PCB
    for sx in (-1, 1):
        for sy in (-1, 1):
            u, v = W / 2 + sx * SCR_HOLE_DX / 2, SCR_V + sy * SCR_HOLE_DY / 2
            panel = panel.union(pcyl(u, v, SCR_STANDOFF_D, PT - 0.1, PT + GLASS_TO_PCB))
            panel = panel.cut(pcyl(u, v, SCR_PILOT_D, PT, PT + GLASS_TO_PCB + 1))
    # pocket for the part below the glass in the maker's model (camera position)
    panel = panel.cut(pbox(W / 2 - 7, W / 2 + 7, SCR_V - 92, SCR_V - GLASS_H / 2 + 1, PT - 1.5, PT + 1))
    for u, v in PANEL_SCREWS:
        panel = panel.cut(pcyl(u, v, SCREW_D, -1, PT + 1))
    return panel


def make_strap():
    s = box(0.5, POST_X + POST_SX / 2, POST_Y - 5, POST_Y + 5, STACK_TOP + 0.5, STACK_TOP + 0.5 + STRAP_T)
    return s.cut(zcyl(POST_X, POST_Y, SCREW_D, STACK_TOP, STACK_TOP + 10))


def make_spacer():
    z0 = FL + PSU_H
    ring = box(PSU_X0, PSU_X0 + PSU_W, PSU_Y0, PSU_Y0 + PSU_L, z0, z0 + PSU_GAP)
    return ring.cut(box(PSU_X0 + 8, PSU_X0 + PSU_W - 8, PSU_Y0 + 8, PSU_Y0 + PSU_L - 8, z0 - 1, z0 + PSU_GAP + 1))


def dummies():
    """Stand-ins for the purchased parts, for the preview and the collision check."""
    out = []
    for k in range(2):
        z0 = FL + k * (PSU_H + PSU_GAP)
        out.append(box(PSU_X0, PSU_X0 + PSU_W, PSU_Y0, PSU_Y0 + PSU_L, z0, z0 + PSU_H))
    z = FL + PCB_STANDOFF
    out.append(box(PCB_X0, PCB_X0 + PCB_W, PCB_Y0, PCB_Y0 + PCB_D, z, z + 1.6))
    out.append(box(PCB_X0 + 62, PCB_X0 + 92, PCB_Y0 + 20, PCB_Y0 + 88, z + 1.6, z + 22))   # DevKit
    for x0 in (PCB_X0 + 2, PCB_X0 + PCB_W - 10):                                            # terminals
        out.append(box(x0, x0 + 8, PCB_Y0 + 5, PCB_Y0 + 80, z + 1.6, z + 16))
    z = FL + RAIL_BOSS_H
    out.append(box(RAIL_X0, RAIL_X0 + RAIL_LEN, RAIL_Y - 17.5, RAIL_Y + 17.5, z, z + 7.5))      # rail
    out.append(box(RAIL_X0 + 2, RAIL_X0 + RAIL_LEN - 2, RAIL_Y - TERMINAL_LEN / 2, RAIL_Y + TERMINAL_LEN / 2,
                   z + 7.5, z + TERMINAL_H))                                                     # rail parts
    for i in range(6):                                                                          # M16 bodies
        out.append(ycyl(M16_X0 + i * M16_PITCH, M16_Z, 20.0, D - T - 28, D - T))
    out.append(box(IEC_X - IEC_W / 2, IEC_X + IEC_W / 2, D - T - 27.4, D - T,
                   IEC_Z - IEC_H / 2, IEC_Z + IEC_H / 2))                                        # mains inlet
    # screen: glass + LCD, PCB, and a box for everything behind the PCB (conservative)
    out.append(pbox(W / 2 - GLASS_W / 2, W / 2 + GLASS_W / 2, SCR_V - GLASS_H / 2, SCR_V + GLASS_H / 2,
                    PT, PT + GLASS_TO_PCB))
    out.append(pbox(W / 2 - SCR_PCB_W / 2, W / 2 + SCR_PCB_W / 2, SCR_V - SCR_PCB_H / 2, SCR_V + SCR_PCB_H / 2,
                    PT + GLASS_TO_PCB, PT + SCR_DEPTH))
    return out


def flat_panel(panel):
    """Front panel laid on its outer face for printing."""
    return panel.rotate((0, 0, 0), (1, 0, 0), -(90 - TILT))


def compound(items):
    return cq.Compound.makeCompound([i.val() for i in items])


if __name__ == "__main__":
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.startswith("cabinet_"):
            os.remove(os.path.join(out, f))
    body, lid, panel = make_body(), make_lid(), make_panel()
    strap, spacer = make_strap(), make_spacer()
    parts = {"body": body, "lid": lid.translate((0, 0, -H)), "front_panel": flat_panel(panel),
             "psu_strap": strap, "psu_spacer": spacer}
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
    view([body, lid, panel], "rear", 205, 25)
    view([body, lid, panel], "front", -30, 20)
    bb = compound([body, lid, panel]).BoundingBox()
    print("overall", round(bb.xlen, 1), round(bb.ylen, 1), round(bb.zlen, 1),
          "| body height", round(H, 1), "setback", round(SETBACK, 1))
