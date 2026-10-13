"""Visual pattern editor.

A QGraphicsView showing a horizontal "paper" with 4 stacked lanes (one per
gun). Each pattern element is a draggable segment with two endpoint handles.
For dots patterns the spacing is rendered as evenly-spaced dots inside the
segment, and adjustable with the mouse wheel.

Coordinates: scene X = millimetres * px-per-mm; scene Y = lane_index * LANE_H.
All mm <-> px conversion is funnelled through `mm_to_px` / `px_to_mm`.  The
scale is set on every scene rebuild so the canvas fills the view width (a
short pattern is shown large); it never changes in the middle of a drag.
"""
from __future__ import annotations

from typing import Callable

from PySide6.QtCore import QPointF, QRectF, Qt, QTimer, Signal
from PySide6.QtGui import (QBrush, QColor, QCursor, QFont, QPainter, QPen,
                           QWheelEvent)
from PySide6.QtWidgets import (QGraphicsItem, QGraphicsScene, QGraphicsView,
                               QMenu, QWidget)

from app import protocol as proto

# -----------------------------------------------------------------------------
# Layout constants
# -----------------------------------------------------------------------------
LANE_H              = 110
LANE_PAD            = 8
HANDLE_R            = 7
LABEL_STRIP_H       = 18                  # space above body for mm labels
LANE_TITLE_H        = 14                  # space at lane top for "אקדח N"
SEG_BODY_HEIGHT     = LANE_H - 2 * LANE_PAD - LANE_TITLE_H - LABEL_STRIP_H
SNAP_MM             = 0.5
# Canvas (display only — never exposed to the user as "paper length").  We
# auto-size to fit the rightmost segment plus a margin (20 % of it, 10..100 mm),
# but never below MIN_CANVAS_MM and never above MAX_CANVAS_MM (defensive cap).
MIN_CANVAS_MM       = 100.0
CANVAS_MARGIN_MM    = 100.0               # largest margin
MAX_CANVAS_MM       = 5000.0
RULER_MIN_GAP_PX    = 45                  # ruler labels at least this far apart
MIN_PX_PER_MM       = 1.6                 # below this, scroll instead of shrinking
DEFAULT_NEW_LEN_MM  = 30.0
DEFAULT_SPACING_MM  = 5.0

GUN_COLORS = [
    QColor("#3b82f6"),  # blue
    QColor("#10b981"),  # emerald
    QColor("#f59e0b"),  # amber
    QColor("#ef4444"),  # red
]


# Current scale; set by PatternEditorView._rebuild_scene (one editor exists).
_px_per_mm = MIN_PX_PER_MM


def mm_to_px(mm: float) -> float:
    return mm * _px_per_mm


def px_to_mm(px: float) -> float:
    return px / _px_per_mm


def snap(mm: float) -> float:
    return round(mm / SNAP_MM) * SNAP_MM


# -----------------------------------------------------------------------------
# Items
# -----------------------------------------------------------------------------

class HandleItem(QGraphicsItem):
    """Endpoint handle. Side is 'left' or 'right'; moves only that endpoint."""

    def __init__(self, parent: "SegmentItem", side: str) -> None:
        super().__init__(parent)
        self._side = side
        self.setFlag(QGraphicsItem.ItemIsSelectable, False)
        self.setAcceptHoverEvents(True)
        self.setCursor(QCursor(Qt.SizeHorCursor))
        self.setZValue(2)

    def boundingRect(self) -> QRectF:  # noqa: N802
        r = HANDLE_R
        return QRectF(-r, -r, 2 * r, 2 * r)

    def paint(self, p: QPainter, *_a) -> None:
        # Hollow ring: a dot (or line) under the handle stays visible.  The
        # whole circle is still grabbable (shape = bounding rect).
        p.setRenderHint(QPainter.Antialiasing)
        p.setBrush(Qt.NoBrush)
        p.setPen(QPen(QColor("#0f1318"), 3.5))          # dark edge for contrast
        p.drawEllipse(QPointF(0, 0), HANDLE_R, HANDLE_R)
        p.setPen(QPen(QColor("#e6e9ee"), 2.0))
        p.drawEllipse(QPointF(0, 0), HANDLE_R, HANDLE_R)

    def mousePressEvent(self, ev):  # noqa: N802
        if ev.button() == Qt.LeftButton:
            ev.accept()
        else:
            super().mousePressEvent(ev)

    def mouseMoveEvent(self, ev):  # noqa: N802
        seg: SegmentItem = self.parentItem()  # type: ignore[assignment]
        new_mm = snap(px_to_mm(ev.scenePos().x()))
        if self._side == "left":
            seg.set_start(new_mm, commit=False)
        else:
            seg.set_end(new_mm, commit=False)

    def mouseReleaseEvent(self, ev):  # noqa: N802
        seg: SegmentItem = self.parentItem()  # type: ignore[assignment]
        seg.commit()
        super().mouseReleaseEvent(ev)


class SegmentItem(QGraphicsItem):
    """One pattern element on a gun lane."""

    def __init__(self,
                 lane_y: float,
                 elem: proto.PatternElement,
                 ptype_getter: Callable[[], proto.PatternType],
                 on_committed: Callable[[], None],
                 on_request_delete: Callable[["SegmentItem"], None],
                 siblings_getter: Callable[[], list["SegmentItem"]],
                 color: QColor) -> None:
        super().__init__()
        self._lane_y      = lane_y
        self.elem         = elem
        self._ptype_get   = ptype_getter
        self._on_commit   = on_committed
        self._on_delete   = on_request_delete
        self._siblings    = siblings_getter
        self._color       = color
        self._drag_anchor: float | None = None  # scene X at press for body drag
        self._drag_start_mm: float = 0.0
        self._drag_end_mm:   float = 0.0
        self._drag_left_lim:  float = 0.0
        self._drag_right_lim: float = MAX_CANVAS_MM

        self.setFlag(QGraphicsItem.ItemIsSelectable, True)
        self.setAcceptHoverEvents(True)
        self.setCursor(QCursor(Qt.SizeAllCursor))
        self.setZValue(1)

        self.h_left  = HandleItem(self, "left")
        self.h_right = HandleItem(self, "right")
        self._sync_handles()

    # ---- geometry ----------------------------------------------------------
    def _body_rect(self) -> QRectF:
        x0 = mm_to_px(self.elem.start_mm)
        x1 = mm_to_px(self.elem.end_mm)
        # body sits below the lane title and the label strip
        y  = self._lane_y + LANE_PAD + LANE_TITLE_H + LABEL_STRIP_H
        return QRectF(x0, y, max(1.0, x1 - x0), SEG_BODY_HEIGHT)

    def _label_rect(self) -> QRectF:
        body = self._body_rect()
        return QRectF(body.left() - 60, body.top() - LABEL_STRIP_H,
                      body.width() + 120, LABEL_STRIP_H)

    def boundingRect(self) -> QRectF:  # noqa: N802
        return self._body_rect().united(self._label_rect()) \
            .adjusted(-HANDLE_R - 2, -2, +HANDLE_R + 2, +2)

    def _sync_handles(self) -> None:
        r = self._body_rect()
        self.h_left .setPos(r.left(),  r.center().y())
        self.h_right.setPos(r.right(), r.center().y())

    # ---- paint -------------------------------------------------------------
    def paint(self, p: QPainter, *_a) -> None:
        p.setRenderHint(QPainter.Antialiasing)
        r = self._body_rect()
        is_dots = self._ptype_get() == proto.PatternType.DOTS

        # ---- mm labels ABOVE the segment, in the label strip --------------
        f = QFont(); f.setPointSize(8); p.setFont(f)
        p.setPen(QPen(QColor("#e6e9ee")))

        start_txt  = f"{self.elem.start_mm:.1f}"
        end_txt    = f"{self.elem.end_mm:.1f}"
        length_mm  = self.elem.end_mm - self.elem.start_mm
        center_txt = f"{length_mm:.1f} mm"
        if is_dots:
            center_txt += f"  ·  Δ {self.elem.spacing_mm:.1f}"

        # Start at left handle
        p.drawText(QRectF(r.left() - 40, r.top() - LABEL_STRIP_H,
                          80, LABEL_STRIP_H - 2),
                   Qt.AlignHCenter | Qt.AlignBottom, start_txt)
        # End at right handle
        p.drawText(QRectF(r.right() - 40, r.top() - LABEL_STRIP_H,
                          80, LABEL_STRIP_H - 2),
                   Qt.AlignHCenter | Qt.AlignBottom, end_txt)
        # Length / spacing in the middle (only if there's room)
        if r.width() > 80:
            mid_pen = QPen(QColor("#9ca6b3"))
            p.setPen(mid_pen)
            p.drawText(QRectF(r.left(), r.top() - LABEL_STRIP_H,
                              r.width(), LABEL_STRIP_H - 2),
                       Qt.AlignHCenter | Qt.AlignBottom, center_txt)

        # ---- body ---------------------------------------------------------
        border = QPen(self._color.darker(140), 1.5)
        fill   = QColor(self._color); fill.setAlpha(60 if is_dots else 180)
        p.setPen(border)
        p.setBrush(QBrush(fill))
        p.drawRoundedRect(r, 4, 4)

        if is_dots and self.elem.spacing_mm > 0.0:
            p.setPen(Qt.NoPen)
            p.setBrush(QBrush(self._color))
            dot_r = 4.0
            cy = r.center().y()
            x_mm = self.elem.start_mm
            # Cap how many dots we draw if spacing is silly-small.
            max_dots = 600
            i = 0
            while x_mm <= self.elem.end_mm + 1e-6 and i < max_dots:
                cx = mm_to_px(x_mm)
                p.drawEllipse(QPointF(cx, cy), dot_r, dot_r)
                x_mm += self.elem.spacing_mm
                i += 1

    # ---- overlap limits (siblings in the same lane) ------------------------
    def _left_limit(self) -> float:
        """Lowest start_mm allowed for this segment: the right edge of the
        nearest sibling sitting to our left (0 if none)."""
        lim = 0.0
        for s in self._siblings():
            if s is self:
                continue
            if s.elem.end_mm <= self.elem.start_mm + 1e-6:
                lim = max(lim, s.elem.end_mm)
        return lim

    def _right_limit(self) -> float:
        """Highest end_mm allowed for this segment: the left edge of the
        nearest sibling sitting to our right (MAX_CANVAS_MM if none)."""
        lim = MAX_CANVAS_MM
        for s in self._siblings():
            if s is self:
                continue
            if s.elem.start_mm >= self.elem.end_mm - 1e-6:
                lim = min(lim, s.elem.start_mm)
        return lim

    # ---- mutation ----------------------------------------------------------
    def set_start(self, mm: float, commit: bool) -> None:
        mm = max(self._left_limit(), min(mm, self.elem.end_mm - SNAP_MM))
        self.prepareGeometryChange()
        self.elem.start_mm = mm
        self._sync_handles()
        self.update()
        if commit:
            self._on_commit()

    def set_end(self, mm: float, commit: bool) -> None:
        mm = max(self.elem.start_mm + SNAP_MM,
                 min(mm, self._right_limit()))
        self.prepareGeometryChange()
        self.elem.end_mm = mm
        self._sync_handles()
        self.update()
        if commit:
            self._on_commit()

    def commit(self) -> None:
        self._on_commit()

    # ---- body drag (move whole segment) ------------------------------------
    def mousePressEvent(self, ev):  # noqa: N802
        if ev.button() == Qt.LeftButton:
            self._drag_anchor   = ev.scenePos().x()
            self._drag_start_mm = self.elem.start_mm
            self._drag_end_mm   = self.elem.end_mm
            # Freeze the gap we are allowed to move within (neighbour edges).
            self._drag_left_lim  = self._left_limit()
            self._drag_right_lim = self._right_limit()
            ev.accept()
        else:
            super().mousePressEvent(ev)

    def mouseMoveEvent(self, ev):  # noqa: N802
        if self._drag_anchor is None:
            return
        d_mm = px_to_mm(ev.scenePos().x() - self._drag_anchor)
        length = self._drag_end_mm - self._drag_start_mm
        new_start = snap(self._drag_start_mm + d_mm)
        hi = min(MAX_CANVAS_MM, self._drag_right_lim) - length
        new_start = max(self._drag_left_lim, min(new_start, hi))
        self.prepareGeometryChange()
        self.elem.start_mm = new_start
        self.elem.end_mm   = new_start + length
        self._sync_handles()
        self.update()

    def mouseReleaseEvent(self, ev):  # noqa: N802
        if self._drag_anchor is not None:
            self._drag_anchor = None
            self._on_commit()
        super().mouseReleaseEvent(ev)

    # ---- wheel adjusts spacing (Dots only) ---------------------------------
    def wheelEvent(self, ev):  # noqa: N802
        if self._ptype_get() != proto.PatternType.DOTS:
            return
        step = SNAP_MM if ev.delta() > 0 else -SNAP_MM
        new_sp = max(SNAP_MM, self.elem.spacing_mm + step)
        self.elem.spacing_mm = round(new_sp / SNAP_MM) * SNAP_MM
        self.update()
        self._on_commit()
        ev.accept()

    # ---- context menu (delete) ---------------------------------------------
    def contextMenuEvent(self, ev):  # noqa: N802
        m = QMenu()
        act_del = m.addAction("מחק מקטע")
        chosen = m.exec(ev.screenPos())
        if chosen == act_del:
            self._on_delete(self)


# -----------------------------------------------------------------------------
# View
# -----------------------------------------------------------------------------

class PatternEditorView(QGraphicsView):
    """Multi-lane visual pattern editor. One instance edits all 4 guns."""

    pattern_committed = Signal(int)  # gun index 0-based

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)

        self._scene = QGraphicsScene(self)
        self._scene.setBackgroundBrush(QBrush(QColor("#181b1f")))
        self.setScene(self._scene)
        self.setRenderHint(QPainter.Antialiasing)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarAsNeeded)
        self.setVerticalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.setTransformationAnchor(QGraphicsView.AnchorUnderMouse)

        # Per-gun state
        self._gun_types:  list[proto.PatternType] = [
            proto.PatternType.NONE for _ in range(proto.NUM_GUNS)]
        self._segments:   list[list[SegmentItem]] = [
            [] for _ in range(proto.NUM_GUNS)]
        # Cached canvas length in mm.  Recomputed on every commit; if it
        # changes meaningfully we rebuild the scene (cheap -- a handful of
        # ruler items + per-segment items, well under a millisecond).
        self._cur_canvas_mm: float = MIN_CANVAS_MM

        self._rebuild_scene()
        self.pattern_committed.connect(self._maybe_resize_canvas)

    # ---- public API --------------------------------------------------------
    def set_gun_type(self, gun_idx: int, ptype: proto.PatternType) -> None:
        self._gun_types[gun_idx] = ptype
        if ptype == proto.PatternType.NONE:
            self.clear_gun(gun_idx, emit=False)
        for s in self._segments[gun_idx]:
            if ptype == proto.PatternType.DOTS and s.elem.spacing_mm <= 0.0:
                s.elem.spacing_mm = DEFAULT_SPACING_MM
            s.update()
        self.pattern_committed.emit(gun_idx)

    def gun_type(self, gun_idx: int) -> proto.PatternType:
        return self._gun_types[gun_idx]

    def add_segment(self, gun_idx: int, start_mm: float | None = None) -> None:
        if self._gun_types[gun_idx] == proto.PatternType.NONE:
            return
        if start_mm is None:
            # Drop the new segment just past the current rightmost extent
            # of this gun's existing segments, so segments stack left->right.
            existing_end = max(
                (s.elem.end_mm for s in self._segments[gun_idx]),
                default=0.0)
            start_mm = existing_end + 20.0 if existing_end > 0 else 50.0
        # Never create a segment that overlaps an existing one: slide the
        # requested start right to the first free gap that fits.
        free_start = self._find_free_start(
            gun_idx, max(0.0, start_mm), DEFAULT_NEW_LEN_MM)
        if free_start is None:
            return  # no room before MAX_CANVAS_MM
        start = snap(free_start)
        spacing = DEFAULT_SPACING_MM if (
            self._gun_types[gun_idx] == proto.PatternType.DOTS) else 0.0
        elem = proto.PatternElement(start_mm=start,
                                    end_mm=start + DEFAULT_NEW_LEN_MM,
                                    spacing_mm=spacing)
        self._add_segment_item(gun_idx, elem)
        self.pattern_committed.emit(gun_idx)

    def _find_free_start(self, gun_idx: int, start: float,
                         length: float) -> float | None:
        """Return the first start >= `start` where a `length`-mm segment fits
        without overlapping existing segments, or None if it cannot fit
        before MAX_CANVAS_MM."""
        occupied = sorted((s.elem.start_mm, s.elem.end_mm)
                          for s in self._segments[gun_idx])
        cand = start
        moved = True
        while moved:
            moved = False
            for a, b in occupied:
                if cand < b and cand + length > a:   # overlaps [a, b]
                    cand = b                         # jump past it
                    moved = True
            if cand + length > MAX_CANVAS_MM:
                return None
        return cand

    def clear_gun(self, gun_idx: int, emit: bool = True) -> None:
        for s in self._segments[gun_idx]:
            self._scene.removeItem(s)
        self._segments[gun_idx] = []
        if emit:
            self.pattern_committed.emit(gun_idx)

    def load_pattern(self, gun_idx: int, ptype: proto.PatternType,
                     elements: list[proto.PatternElement]) -> None:
        self.clear_gun(gun_idx, emit=False)
        self._gun_types[gun_idx] = ptype
        for elem in elements:
            self._add_segment_item(gun_idx, elem)
        # Resync the canvas to whatever the loaded patterns now require.
        self._maybe_resize_canvas(gun_idx)

    def export_pattern(self, gun_idx: int
                       ) -> tuple[proto.PatternType, list[proto.PatternElement]]:
        return (self._gun_types[gun_idx],
                [s.elem for s in self._segments[gun_idx]])

    # ---- internals ---------------------------------------------------------
    def _lane_y(self, gun_idx: int) -> float:
        return gun_idx * LANE_H + 24  # 24px reserved for top ruler

    def _clamp_canvas(self, max_end: float) -> float:
        """Smallest canvas wide enough to show `max_end` plus margin, clamped
        to [MIN_CANVAS_MM, MAX_CANVAS_MM] and rounded up (10 mm below 500 mm,
        else 50 mm) so the ruler ticks line up neatly."""
        margin = min(CANVAS_MARGIN_MM, max(10.0, 0.2 * max_end))
        target = max(MIN_CANVAS_MM, max_end + margin)
        target = min(MAX_CANVAS_MM, target)
        unit = 10.0 if target < 500.0 else 50.0
        return float(int((target + unit - 0.001) / unit) * unit)

    def _fit_scale(self, canvas_mm: float) -> float:
        """px per mm so `canvas_mm` fills the view width; long canvases keep
        the old 1.6 px/mm and scroll instead of squeezing the labels."""
        avail = max(200, self.viewport().width() - 30)
        return max(MIN_PX_PER_MM, avail / canvas_mm)

    def resizeEvent(self, ev) -> None:  # noqa: N802
        super().resizeEvent(ev)
        # Refit after a window resize (deferred, like other rebuilds).
        if abs(self._fit_scale(self._cur_canvas_mm) - _px_per_mm) > 0.01 * _px_per_mm:
            QTimer.singleShot(0, self._rebuild_scene)

    def _canvas_length_mm(self) -> float:
        max_end = 0.0
        for segs in self._segments:
            for s in segs:
                if s.elem.end_mm > max_end:
                    max_end = s.elem.end_mm
        return self._clamp_canvas(max_end)

    def _maybe_resize_canvas(self, _gun_idx: int) -> None:
        desired = self._canvas_length_mm()
        if abs(desired - self._cur_canvas_mm) >= 1.0:
            # Defer the rebuild: this slot can fire from inside a segment's
            # mouse-release handler, and _rebuild_scene() clears the scene
            # (deleting the very item whose event is still on the stack).
            # Running it on the next event-loop tick keeps Qt happy.
            QTimer.singleShot(0, self._rebuild_scene)

    def _add_segment_item(self, gun_idx: int,
                          elem: proto.PatternElement) -> None:
        color = GUN_COLORS[gun_idx]

        def on_commit() -> None:
            self.pattern_committed.emit(gun_idx)

        def on_delete(item: SegmentItem) -> None:
            try:
                self._segments[gun_idx].remove(item)
            except ValueError:
                return
            self._scene.removeItem(item)
            self.pattern_committed.emit(gun_idx)

        seg = SegmentItem(
            lane_y=self._lane_y(gun_idx),
            elem=elem,
            ptype_getter=lambda gi=gun_idx: self._gun_types[gi],
            on_committed=on_commit,
            on_request_delete=on_delete,
            siblings_getter=lambda gi=gun_idx: self._segments[gi],
            color=color,
        )
        self._scene.addItem(seg)
        self._segments[gun_idx].append(seg)

    def _rebuild_scene(self) -> None:
        # Remember current segments before clearing.
        snap_state = [(self._gun_types[i],
                       [(s.elem.start_mm, s.elem.end_mm, s.elem.spacing_mm)
                        for s in self._segments[i]])
                      for i in range(proto.NUM_GUNS)]
        self._scene.clear()
        self._segments = [[] for _ in range(proto.NUM_GUNS)]

        # Compute canvas size from the SNAPSHOT (self._segments is now empty,
        # so reading it here would always collapse back to MIN_CANVAS_MM).
        max_end = 0.0
        for _ptype, elems in snap_state:
            for _s, e_mm, _sp in elems:
                if e_mm > max_end:
                    max_end = e_mm
        canvas_mm = self._clamp_canvas(max_end)
        self._cur_canvas_mm = canvas_mm
        global _px_per_mm
        _px_per_mm = self._fit_scale(canvas_mm)
        w = mm_to_px(canvas_mm)
        h = self._lane_y(proto.NUM_GUNS) + 8
        self._scene.setSceneRect(QRectF(-10, -4, w + 20, h + 8))

        # Ruler at top
        ruler_pen   = QPen(QColor("#3a414b"), 1)
        tick_pen    = QPen(QColor("#6b727c"), 1)
        text_color  = QColor("#9ca6b3")
        font = QFont(); font.setPointSize(8)

        ruler_y = 18
        self._scene.addLine(0, ruler_y, w, ruler_y, ruler_pen)
        step = next((s for s in (5, 10, 20, 25, 50, 100, 200, 250, 500, 1000)
                     if s * _px_per_mm >= RULER_MIN_GAP_PX), 1000)
        for mm in range(0, int(canvas_mm) + 1, step):
            x = mm_to_px(mm)
            self._scene.addLine(x, ruler_y - 4, x, ruler_y, tick_pen)
            t = self._scene.addText(f"{mm}", font)
            t.setDefaultTextColor(text_color)
            t.setPos(x - 10, ruler_y - 22)

        # Lanes
        for i in range(proto.NUM_GUNS):
            y = self._lane_y(i)
            lane = self._scene.addRect(
                QRectF(0, y, w, LANE_H - LANE_PAD),
                QPen(QColor("#2d333b"), 1),
                QBrush(QColor("#1e2227")))
            lane.setZValue(-1)
            label = self._scene.addText(f"אקדח {i + 1}", font)
            label.setDefaultTextColor(GUN_COLORS[i])
            label.document().setDocumentMargin(6)
            label.setPos(8, y + 2)

        # Re-add segments from snapshot
        for i, (ptype, elems) in enumerate(snap_state):
            self._gun_types[i] = ptype
            for s_mm, e_mm, sp_mm in elems:
                self._add_segment_item(
                    i, proto.PatternElement(s_mm, e_mm, sp_mm))

    # ---- view interactions -------------------------------------------------
    def wheelEvent(self, ev: QWheelEvent) -> None:  # noqa: N802
        # If wheel happened over a segment, that segment handles it (above).
        # Otherwise: horizontal scroll.
        item = self.itemAt(ev.position().toPoint())
        if isinstance(item, (SegmentItem, HandleItem)):
            super().wheelEvent(ev)
            return
        bar = self.horizontalScrollBar()
        bar.setValue(bar.value() - ev.angleDelta().y())
        ev.accept()

    def set_editable(self, on: bool) -> None:
        """Locked (outside admin mode): view only.  Non-interactive also stops
        drags, wheel spacing and the right-click delete menu on segments."""
        self.setInteractive(on)

    def mouseDoubleClickEvent(self, ev):  # noqa: N802
        if not self.isInteractive():
            return
        # Add a new element on the lane that was double-clicked.
        scene_pt = self.mapToScene(ev.position().toPoint())
        for i in range(proto.NUM_GUNS):
            y = self._lane_y(i)
            if y <= scene_pt.y() <= y + LANE_H - LANE_PAD:
                self.add_segment(i, start_mm=px_to_mm(scene_pt.x()))
                return
        super().mouseDoubleClickEvent(ev)
