# Cabinet model (draft 1)

`cabinet.py` is a parametric CadQuery model (`pip install cadquery`, then
`python3 cabinet.py`).  It writes STEP and STL files and line-drawing
previews into `out/`.  Edit the constants at the top of the script and
re-run; do not edit the files in `out/`.

Nothing has been printed or test-fitted yet.

## What it is

Table-top console, 270 x 220 mm, 80 mm high at the front and 115 mm at the
rear.  The lid is a flat 3 mm plate at 9 degrees, held by six M3 screws
into heat-set inserts, and is the part that gets replaced when the touch
screen is added (up to 10.1 inch).  All connectors are on the rear wall.

| File in `out/` | Part | Print |
| -------------- | ---- | ----- |
| `cabinet_body` | Box with floor, bosses, PCB standoffs, rear cut-outs, vents | Floor on the bed, no supports expected |
| `cabinet_lid` | Lid (exported lying flat) | Flat |
| `cabinet_psu_spacer` | 10 mm frame between the two stacked supplies | Flat |
| `cabinet_psu_strap` | Bar that clamps the supply stack; tab goes into the left wall, M3 screw into the post | Flat |

Inside, seen from the front: the two supplies are stacked at the left, the
controller board is on four standoffs at the right, and behind the board
is a 120 mm piece of 35 mm DIN rail (two M3 bosses) for the fuse terminals
and the 0 V / +24 V / PE terminals.  Gun, eye-signal and encoder-signal
wires go straight from the rear sockets to the board's screw terminals.

## To verify before printing

- `M16_HOLE_D` (16.2): mounting hole of the chosen M16 socket.
- `IEC_W`, `IEC_H` (27.5 x 47.5): cut-out of the chosen mains inlet.
- USB feed-through: modelled as a "D" type (24 mm hole, two screws).
- Supply fixing: the stack is located by ribs and clamped by the strap.
  Check against a real LRS-100-24, including access to the lower supply's
  terminal screws (wire it before the upper supply goes on).
- Room under the lid above the supplies is 9 to 20 mm: enough for a bare
  10.1 inch panel, to be rechecked when the screen is chosen.
- DIN terminals are modelled as a 96 x 60 x 60 mm envelope; check against
  the chosen fuse terminals.  The 24 V to 5 V converter has no mounting
  point yet (it can go on the rail).
- Material: PETG, ABS or ASA.
