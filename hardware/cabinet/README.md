# Cabinet model (draft 2)

`cabinet.py` is a parametric CadQuery model (`pip install cadquery`, then
`python3 cabinet.py`).  It writes STEP and STL files into `out/` (and SVG
previews, not committed).  Edit the constants at the top of the script and
re-run; do not edit the files in `out/`.

Nothing has been printed or test-fitted yet.

## What it is

Table-top box for a standing operator, 270 x 250 mm, 177 mm high.  The
touch screen (Elecrow CrowPanel Advanced 10.1) sits in a separate front
panel that leans back 15 degrees from vertical.  The top is a flat lid.
All cables leave through the rear wall.

| File in `out/` | Part | Print |
| -------------- | ---- | ----- |
| `cabinet_body` | Box: floor, side and rear walls, bosses, standoffs, rear cut-outs, vents | Floor on the bed |
| `cabinet_front_panel` | Front panel with the screen window and the screen standoffs (exported lying on its outer face) | Outer face on the bed |
| `cabinet_lid` | Flat top lid | Flat |
| `cabinet_psu_spacer` | 10 mm frame between the two stacked supplies | Flat |
| `cabinet_psu_strap` | Bar that clamps the supply stack | Flat |

Screen: dimensions from the maker's STEP model.  The glass (235 x 143)
sits against the inside of the panel behind a 1.5 mm lip, so it is recessed
3 mm and the panel frame takes knocks.  The screen PCB is held by four
self-tapping M3 screws into standoffs on the panel (holes 241 x 141).  A
stick-on screen protector is recommended against glue.

Inside: supplies stacked at the left, controller board on four standoffs
at the right, 155 mm DIN rail behind them (fuse terminals, 0 V / +24 V / PE
terminals, R1, DDR-30G-5).  Everything inside was moved 30 mm back to clear
the screen.  The model checks that no part touches another.

Fasteners: M3 heat-set inserts in the body (17 pockets), M3 screws for the
lid, front panel, board, rail and strap; 4 self-tapping M3 for the screen.

## To verify before printing

- `INSERT_D`: pocket diameter for the inserts actually bought.
- Mains inlet Bulgin BZV01/Z0000/10: cut-out 34.4 x 70.1 is the catalogue
  minimum.  Panel thickness for the /10 version is not known yet; the
  wall is 3 mm and can be thinned around the cut-out.
- USB feed-through: modelled as a D-type 24 mm hole with screws 19 x 24.
- Screen holes 241 x 141 and the window position: check on the real board.
- Rail parts are modelled as a 151 x 60 x 60 mm envelope.
- Material: PETG, ABS or ASA.
