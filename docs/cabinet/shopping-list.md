# Shopping list (draft 1)

Links were found by web search on 2026-11-16.  Price, stock and shipping to
Israel were NOT checked: open each link and confirm before ordering.  The
search snippets showed the Lumberg sockets as out of stock at TME at some
point, so check stock first; the binder parts are the equivalent.

Most items are from one distributor (TME, Poland, ships to Israel) to pay
shipping once.  Generic parts are cheaper on AliExpress.

## A. One order at TME

| # | Item | Qty | Part | Link |
| - | ---- | --- | ---- | ---- |
| 1 | Gun socket, M16 5-pole 240 deg, female, panel | 4 (+1 spare) | Lumberg 0304 05-1 | https://www.tme.eu/en/details/0304-05-1/m16-connectors/lumberg/0304-05-1/ |
| 2 | Encoder socket, M16 5-pole 180 deg, female, panel | 1 | Lumberg 0304 05 | https://www.tme.eu/en/details/0304-05/m16-connectors/lumberg/0304-05/ |
| 3 | Eye socket, M16 3-pole, female, panel | 1 | Lumberg 0304 03 | https://www.tme.eu/en/details/0304-03/m16-connectors/lumberg/0304-03/ |
| 4 | Power supply 24 V 4.5 A | 2 | Mean Well LRS-100-24 | https://www.tme.eu/en/details/lrs-100-24/built-in-power-supplies/mean-well/ |
| 5 | Mains inlet C14, switch, fuse 5x20, filter, snap-in | 1 | Schaffner FN370-2-21 | https://www.tme.eu/html/EN/socket-ac-supply-c14-e-schaffner-te-connectivity/ramka_35117_EN_pelny.html |
| 6 | USB panel feed-through, D type | 1 | Neutrik NAUSB-W | https://www.tme.eu/en/details/ntr-nausbw/usb-ieee1394-connectors/neutrik/nausb-w/ |
| 7 | Fuse holder 5x20, on cable | 4 | e.g. Bulgin FX0380 | https://www.tme.eu/en/katalog/fuseholders-for-cable_113042/ |
| 8 | Fuses 5x20 slow: 2 A, 4 A, 1 A, 0.5 A | 3 each | any | https://www.tme.eu/en/katalog/fuseholders_100088/ (fuses are in the neighbouring category) |
| 9 | Diode 6 A, >= 100 V, axial | 2 (+2) | 6A10 | search "6A10" at TME |
| 10 | Resistor 1.5 k, 1 W (or 2 W), axial | 1 (+2) | any | search at TME |

If the Lumberg sockets are out of stock: binder 09-0320-00-05 (gun),
09-0316-00-05 (encoder), 09-0308-00-03 (eye).

All M16 lines exist in front-mount and rear-mount versions.  Either works;
the hole in the box is set to the one that arrives.

## B. Generic, AliExpress or a local shop

| Item | Qty | Search words |
| ---- | --- | ------------ |
| M3 brass heat-set inserts, about 4-5 mm long, OD about 4.5-5 mm | 20 | "M3 heat set insert" |
| M3 screws 6 / 8 / 10 mm, pan head | assortment | "M3 screw kit" |
| Lever terminal blocks, 3- and 5-way | 10 | "Wago 221" |
| Wire 0.75 mm2 red / black / blue, 0.25 mm2 for signals, ferrules | - | - |
| USB cable A to the DevKit's connector (micro-B or C), 30 cm | 1 | - |
| USB cable A to B, PC side | 1 | "printer cable" |
| Stick-on rubber feet | 4 | - |
| Mains cable C13 | 1 | - |

## C. Later (touch-screen stage)

24 V to 5 V converter, 3 A, and the screen itself.

## After the parts arrive

Measure and update in `hardware/cabinet/cabinet.py`: `M16_HOLE_D`, `IEC_W`,
`IEC_H`, and the insert pocket `INSERT_D`.
