# Cabinet parts list (draft 1)

Goes with `wiring-diagram.svg`.  Part numbers were taken from distributor
listings and from memory of the manufacturers' catalogues, not from the
datasheets: check the contact-arrangement drawing of every connector before
ordering.  Nothing here has been ordered or tested.

## Panel connectors (mate with the existing hhs cables)

The hhs cables carry M16 threaded male plugs (IEC 61076-2-106).  The box
needs female panel sockets with solder contacts.  Pin arrangements read from
the C-420 manual, pages 37-38:

| Socket | Qty | Arrangement | Lumberg | binder 680 |
| ------ | --- | ----------- | ------- | ---------- |
| Gun 1..4 | 4 | 5-pole, 240 deg, pins numbered in sequence 1-2-3-4-5 (DIN 45322) | 0304 05-1 | 09-0320-00-05 ("5 stereo") |
| ENC | 1 | 5-pole, 180 deg, pin order 1-4-2-5-3 (DIN 41524) | 0304 05 | 09-0316-00-05 |
| Eye (Start) | 1 | 3-pole, 180 deg (DIN 41524) | 0304 03 | 09-0308-00-03 |

The gun and encoder sockets are both 5-pole but do not mate with each
other's plugs, as on the old controller.

Wiring: Gun pin 1 = Gun +, pin 2 = Gun -.  ENC pin 1 = +24 V, 2 = 0 V,
3 = signal.  Eye pin 1 = +24 V, 2 = 0 V, 3 = signal.

Chosen (Mouser cart 2026-11-19, verified on the Amphenol drawing
NA T 3XXX 0XX U): Amphenol Tuchel C091A front-mount female, solder,
T 3363 000 (gun, 05-a, 240 deg) x5, T 3363 010 (encoder, 05-b, 180 deg) x1,
T 3263 000 (eye, 03-a) x1.  Panel cut-out 18 mm + key slot 2.8 mm to
19.1 mm, panel 1-5 mm.  Solder cups take wire up to 0.5 mm2.

## Power

| Item | Qty | Proposal | Notes |
| ---- | --- | -------- | ----- |
| PSU-A, PSU-B | 2 | Mean Well LRS-100-24 (24 V, 4.5 A, 129 x 97 x 30 mm, no fan) | Stacked in series for 48 V.  Old controller was 160 VA in total, so 2 x 108 W leaves margin for the screen. |
| D-A, D-B | 2 | 6A10 (6 A, 1000 V) or similar | Across each supply output, cathode on V+. |
| Mains inlet | 1 | IEC C14 with switch and 5x20 fuse holder | F0 2 A slow. |
| F1 (48 V) | 1 | DIN-rail fuse terminal 5x20, 4 A slow | Provisional. |
| F2 (24 V board) | 1 | DIN-rail fuse terminal 5x20, 1 A slow | |
| F3 (24 V sensors) | 1 | DIN-rail fuse terminal 5x20, 0.5 A | |
| F4 + DC/DC 24 V to 5 V, 3 A | 1 | Model to choose with the screen | Stage 2: touch screen, future 5 V encoder. |

## Small parts

| Item | Qty | Notes |
| ---- | --- | ----- |
| R1 1.5 k, 1 W, with jumper JP1 | 1 | Encoder series resistor for 24 V; JP1 closed for 5 V. |
| USB panel feed-through (to the DevKit's USB port) | 1 | PC link and flashing. |
| 35 mm DIN rail, 120 mm, with 4 fuse terminals (F1-F4), 0 V group, +24 V sensors group, PE terminal | 1 | See shopping-list.md. |
| M3 brass heat-set inserts, M3 screws, 4 PCB standoffs | - | Board holes: see design-inputs.md section 2. |
| Wire: 0.75 mm2 for 48 V and guns, 0.25-0.5 mm2 for signals, 3 x 0.75 mm2 mains | - | |

## Not needed (owner, 2026-11-16)

Flap output, purge buttons, glue level, pressure output, Reset, emergency
stop.

## Order 1 (Mouser cart reviewed 2026-11-19, about 152 USD)

| Item | Part | Qty |
| ---- | ---- | --- |
| Gun socket | Amphenol Tuchel T 3363 000 (C091A, 05-a, 240 deg) | 5 |
| Encoder socket | Amphenol Tuchel T 3363 010 (C091A, 05-b, 180 deg) | 1 |
| Eye socket | Amphenol Tuchel T 3263 000 (C091A, 03-a) | 1 |
| Supplies | Mean Well LRS-100-24 | 2 |
| Mains inlet | Bulgin BZV01/Z0000/10: C14, 2-pole switch, 5x20 fuse holder, snap-in, cut-out 70.1 x 34.4 mm (min); panel thickness depends on the suffix - check before modelling | 1 |
| USB feed-through | Neutrik NAUSB-W (mount with the B socket outside) | 1 |
| 5 V for the screen | Mean Well DDR-30G-5 (DIN rail, 35 mm wide, 9-36 V in, 5 V 6 A) | 1 |
| Diodes | Comchip 6A10-G | 4 |
| R1 | YAGEO FMP100JR-52-1K5 (1.5 k, 1 W) | 3 |

Owner already has: DIN rail, fuse and feed-through terminals, jumpers,
fuses.  Screen: Elecrow CrowPanel Advanced 10.1 (ESP32-P4), ordered from
Elecrow (DDP).
