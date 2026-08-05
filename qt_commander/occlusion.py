"""Occlusion solving for UI snapshots.

Given a saved snapshot (the JSON written by qt_snapshot), compute which
elements are actually visible on screen and which are completely covered
by higher-z opaque elements.  Produces a pruned snapshot: covered
elements are removed from the tree, and surviving elements get a
``visible_ratio`` field (1.0 is omitted) when partially covered.

This is a conservative geometric heuristic, not a pixel-exact render:

- Only axis-aligned rectangles are considered (circles, rounded corners
  and arbitrary QML shapes are approximated by their bounding rect).
- An element occludes only if it is classified as opaque (see
  :func:`is_occluder`); transparent containers (QQuickItem, layouts,
  mouse areas, text, custom QML components) never occlude.
- Elements with equal z_order do not occlude each other (conservative).
- Occlusion is computed per top-level window (topLevelId).

The heuristic errs on the side of keeping elements: only elements fully
covered by an opaque rectangle are removed.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any


# ---------------------------------------------------------------------------
# Rect math
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Rect:
    x: float
    y: float
    w: float
    h: float

    @property
    def area(self) -> float:
        return self.w * self.h

    @property
    def right(self) -> float:
        return self.x + self.w

    @property
    def bottom(self) -> float:
        return self.y + self.h

    def is_empty(self) -> bool:
        return self.w <= 0 or self.h <= 0

    def overlaps(self, other: "Rect") -> bool:
        return (self.x < other.right and other.x < self.right
                and self.y < other.bottom and other.y < self.bottom)

    def bbox(self, other: "Rect") -> "Rect":
        x = min(self.x, other.x)
        y = min(self.y, other.y)
        return Rect(x, y, max(self.right, other.right) - x,
                    max(self.bottom, other.bottom) - y)


def rect_from_node(node: dict) -> Rect | None:
    r = node.get("global_rect")
    if not isinstance(r, dict):
        return None
    try:
        rect = Rect(float(r["x"]), float(r["y"]),
                    float(r["width"]), float(r["height"]))
    except (KeyError, TypeError, ValueError):
        return None
    return rect if not rect.is_empty() else None


def rect_difference(outer: Rect, inner: Rect) -> list[Rect]:
    """outer minus the part overlapped by inner.

    Splits the leftover into up to 4 axis-aligned pieces.
    """
    if not outer.overlaps(inner):
        return [outer]
    # clip inner to outer so the four cuts stay valid
    cx0 = max(inner.x, outer.x)
    cy0 = max(inner.y, outer.y)
    cx1 = min(inner.right, outer.right)
    cy1 = min(inner.bottom, outer.bottom)
    pieces = []
    # top strip
    if cy0 > outer.y:
        pieces.append(Rect(outer.x, outer.y, outer.w, cy0 - outer.y))
    # bottom strip
    if outer.bottom > cy1:
        pieces.append(Rect(outer.x, cy1, outer.w, outer.bottom - cy1))
    # left middle
    if cx0 > outer.x:
        pieces.append(Rect(outer.x, cy0, cx0 - outer.x, cy1 - cy0))
    # right middle
    if outer.right > cx1:
        pieces.append(Rect(cx1, cy0, outer.right - cx1, cy1 - cy0))
    return [p for p in pieces if not p.is_empty()]


class CoveredArea:
    """A normalized (non-overlapping) set of covered rectangles.

    insert() adds an opaque rect; visible_area() returns how much of a
    query rect is NOT covered.  Rects are bucketed by y so every query
    and insert only touches the buckets the rect actually spans --
    O(n) worst case but a small constant on realistic UIs (thousands of
    elements).
    """

    BUCKET = 128.0

    def __init__(self) -> None:
        self._buckets: dict[int, list[Rect]] = {}
        self._bbox: Rect | None = None

    def _touched_buckets(self, rect: Rect) -> list[int]:
        lo = int(rect.y // self.BUCKET)
        hi = int((rect.bottom - 1) // self.BUCKET)
        return list(range(lo, hi + 1))

    def insert(self, rect: Rect) -> None:
        if rect.is_empty():
            return
        for bk in self._touched_buckets(rect):
            for r in self._buckets.get(bk, ()):
                if (r.x <= rect.x and r.y <= rect.y
                        and r.right >= rect.right and r.bottom >= rect.bottom):
                    return  # fully inside an existing piece
        # subtract existing pieces (per bucket) from the new rect
        remaining = [rect]
        for bk in self._touched_buckets(rect):
            for r in self._buckets.get(bk, ()):
                new_remaining = []
                for piece in remaining:
                    if piece.overlaps(r):
                        new_remaining.extend(rect_difference(piece, r))
                    else:
                        new_remaining.append(piece)
                remaining = new_remaining
        for piece in remaining:
            if piece.is_empty():
                continue
            for bk in self._touched_buckets(piece):
                self._buckets.setdefault(bk, []).append(piece)
            self._bbox = (self._bbox.bbox(piece) if self._bbox else piece)

    def visible_area(self, rect: Rect) -> float:
        if rect.is_empty():
            return 0.0
        if self._bbox is None or not rect.overlaps(self._bbox):
            return rect.area
        remaining = [rect]
        for bk in self._touched_buckets(rect):
            if not remaining:
                return 0.0
            for r in self._buckets.get(bk, ()):
                new_remaining = []
                for piece in remaining:
                    if piece.overlaps(r):
                        new_remaining.extend(rect_difference(piece, r))
                    else:
                        new_remaining.append(piece)
                remaining = new_remaining
        return sum(p.area for p in remaining)


# ---------------------------------------------------------------------------
# Opaque-element heuristic
# ---------------------------------------------------------------------------

# QML classes that never paint an opaque background and therefore never
# occlude anything under them.
_TRANSPARENT_QQUICK = {
    "QQuickItem", "QQuickLoader", "QQuickMouseArea", "QQuickText",
    "QQuickRowLayout", "QQuickColumnLayout", "QQuickGridLayout",
    "QQuickStackLayout", "QQuickRow", "QQuickColumn", "QQuickFlow",
    "QQuickGrid", "QQuickFlickable", "QQuickListView", "QQuickGridView",
    "QQuickRepeater", "QQuickContentItem", "QQuickRootItem",
    "QQuickOverlay", "QQuickShaderEffect", "QQuickShaderEffectSource",
    "QQuickWindow", "QQuickItemView", "QQuickScrollBar",
    "QQuickIconImage", "QQuickMnemonicLabel",
}


def is_occluder(className: str) -> bool:
    """Conservative opaqueness test for a snapshot element.

    Widget-family classes (not QQuick-prefixed, not QML-typed) default to
    opaque -- most widgets paint a background.  QML elements occlude only
    when they are rectangles or images; every other QML type (custom
    components, containers, text, layouts) is treated as transparent.
    """
    if not className:
        return False
    if "_QMLTYPE_" in className or className.endswith("_QML_"):
        return False  # custom QML component: transparent by default
    if className.startswith("QQuick"):
        return className not in _TRANSPARENT_QQUICK and (
            "Rectangle" in className or "Image" in className)
    if className.startswith("Q"):
        return True  # widget family
    return False


# ---------------------------------------------------------------------------
# Tree walking
# ---------------------------------------------------------------------------

def iter_nodes(root: dict):
    """Yield (node, parent) for every node in a snapshot tree (incl. root)."""
    yield root, None
    for child in root.get("children", []):
        yield from iter_nodes(child)


def rebuild_tree(root: dict, keep: set[int]) -> dict:
    """Deep-copy the tree keeping only nodes whose objID is in ``keep``."""
    out = dict(root)
    out.pop("children", None)
    kids = root.get("children", [])
    kept = [rebuild_tree(k, keep) for k in kids if k.get("objID") in keep]
    if kept:
        out["children"] = kept
    return out


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def prune_snapshot(snapshot: dict) -> dict:
    """Occlusion-prune a snapshot dict.

    Returns a new snapshot dict with fully covered elements removed.
    Surviving elements that are partially covered get a ``visible_ratio``
    field (0 < ratio < 1; 1.0 is omitted).  A ``pruned`` summary is
    attached: {removed, kept, removed_ratio}.
    """
    nodes = snapshot.get("nodes", [])
    if not isinstance(nodes, list):
        nodes = []
    if not nodes:
        out = dict(snapshot)
        out["nodes"] = []
        out["pruned"] = {"removed": 0, "kept": 0, "removed_ratio": 0.0}
        return out

    # ---- collect all elements with their geometry ---------------------
    elements = []  # (topLevelId, z_order, objID, rect, node)
    kept_ids: set[int] = set()  # no-geometry elements are kept as-is
    for root in nodes:
        for node, _parent in iter_nodes(root):
            oid = node.get("objID")
            if oid is None:
                continue
            rect = rect_from_node(node)
            if rect is None:
                kept_ids.add(oid)  # zero-size / missing geometry: keep
                continue
            z = node.get("z_order", 0)
            try:
                z = float(z)
            except (TypeError, ValueError):
                z = 0.0
            top = node.get("topLevelId", 0)
            elements.append((top, z, oid, rect, node))

    # ---- per window: sort by z DESCENDING, accumulate coverage ---------
    # Higher z renders on top, so it must be registered as an occluder
    # before lower-z elements are queried.  Elements at the same z do not
    # occlude each other (conservative), so process each z level in two
    # passes: query all, then insert all occluders.
    by_window: dict[int, list] = {}
    for e in elements:
        by_window.setdefault(e[0], []).append(e)
    for win_elems in by_window.values():
        win_elems.sort(key=lambda e: e[1], reverse=True)

    covered: dict[int, CoveredArea] = {}
    visible_ratio: dict[int, float] = {}
    for win, win_elems in by_window.items():
        cov = covered.setdefault(win, CoveredArea())
        i = 0
        while i < len(win_elems):
            z = win_elems[i][1]
            j = i
            while j < len(win_elems) and win_elems[j][1] == z:
                j += 1
            # pass 1: query visibility for the whole z level
            for k in range(i, j):
                _top, _z, oid, rect, node = win_elems[k]
                visible = cov.visible_area(rect)
                if visible < rect.area:
                    ratio = visible / rect.area if rect.area > 0 else 0.0
                    visible_ratio[oid] = ratio
            # pass 2: register occluders of this z level
            for k in range(i, j):
                _top, _z, oid, rect, node = win_elems[k]
                if is_occluder(node.get("className", "")):
                    cov.insert(rect)
            i = j

    # ---- decide what to keep ------------------------------------------
    removed = 0
    for _top, _z, oid, rect, node in elements:
        ratio = visible_ratio.get(oid)
        if ratio is not None and ratio <= 0.0:
            removed += 1
            continue
        kept_ids.add(oid)

    # ---- rebuild the tree with annotations ----------------------------
    new_nodes = []
    for root in nodes:
        kept_root = rebuild_tree(root, kept_ids)
        if kept_root.get("objID") in kept_ids:
            new_nodes.append(kept_root)
        elif kept_root.get("children"):
            new_nodes.append(kept_root)  # keep container with visible kids

    # attach visible_ratio annotations (post-rebuild so objIDs match)
    _annotate(new_nodes, visible_ratio)

    out = dict(snapshot)
    out["nodes"] = new_nodes
    out["pruned"] = {
        "removed": removed,
        "kept": len(kept_ids),
        "removed_ratio": round(removed / max(1, removed + len(kept_ids)), 4),
    }
    return out


def _annotate(nodes: list[dict], ratios: dict[int, float]) -> None:
    for node in nodes:
        oid = node.get("objID")
        ratio = ratios.get(oid)
        if ratio is not None and ratio < 1.0:
            node["visible_ratio"] = round(ratio, 4)
        _annotate(node.get("children", []), ratios)
