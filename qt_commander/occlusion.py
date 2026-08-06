"""Occlusion solving for UI snapshots.

Given a saved snapshot (the JSON written by qt_snapshot), compute which
elements a human eye can actually see: fully covered elements are
removed, partially covered ones are kept and annotated with a
``visible_ratio`` field (how much of them remains visible).

This is a geometric heuristic, not a pixel-exact render:

- Only axis-aligned rectangles are considered (circles, rounded corners
  and arbitrary QML shapes are approximated by their bounding rect).
- An element occludes only if it is classified as opaque (see
  :func:`is_occluder`); transparent containers (QQuickItem, layouts,
  mouse areas, text, custom QML components) never occlude, so a text
  label over a background does not hide the background.
- Siblings are ordered by ascending z (ties by declaration order), and
  the whole subtree is treated as one layer: a parent paints before its
  children, so a child (any z) always covers its parent, and a subtree
  paints above its parent's lower-z siblings.  The solve walks the
  resulting paint sequence in reverse.
- A parent never occludes its own descendants (they paint after it);
  a child's rect does occlude its parent's area.
- Removed elements are dropped from the tree; their still-visible
  descendants are reparented one level up.
- Occlusion is computed per top-level window (topLevelId).
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

    insert() adds an opaque rect (with an optional source id);
    visible_area() returns how much of a query rect is NOT covered.
    ``exclude`` lets the caller ignore rectangles from given sources --
    used so a parent container never occludes its own descendants
    (children always paint above their parent in Qt).

    Rects are bucketed by y so every query and insert only touches the
    buckets the rect actually spans.
    """

    BUCKET = 128.0

    def __init__(self) -> None:
        self._buckets: dict[int, list[tuple[Rect, int]]] = {}
        self._bbox: Rect | None = None

    def _touched_buckets(self, rect: Rect) -> list[int]:
        lo = int(rect.y // self.BUCKET)
        hi = int((rect.bottom - 1) // self.BUCKET)
        return list(range(lo, hi + 1))

    def insert(self, rect: Rect, source: int = 0) -> None:
        if rect.is_empty():
            return
        for bk in self._touched_buckets(rect):
            for r, _src in self._buckets.get(bk, ()):
                if (r.x <= rect.x and r.y <= rect.y
                        and r.right >= rect.right and r.bottom >= rect.bottom):
                    return  # fully inside an existing piece
        # subtract existing pieces (per bucket) from the new rect
        remaining = [rect]
        for bk in self._touched_buckets(rect):
            for r, _src in self._buckets.get(bk, ()):
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
                self._buckets.setdefault(bk, []).append((piece, source))
            self._bbox = (self._bbox.bbox(piece) if self._bbox else piece)

    def visible_area(self, rect: Rect, exclude: frozenset[int] = frozenset()) -> float:
        if rect.is_empty():
            return 0.0
        if self._bbox is None or not rect.overlaps(self._bbox):
            return rect.area
        remaining = [rect]
        for bk in self._touched_buckets(rect):
            if not remaining:
                return 0.0
            for r, src in self._buckets.get(bk, ()):
                if src in exclude:
                    continue
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


def is_qml_component(className: str) -> bool:
    """True for custom QML component roots (QGCButton_QMLTYPE_8, ...).

    Used by tests and for documentation; component roots are treated
    like any other element during pruning (a root covered by its own
    background child is removed and the visible child is reparented up).
    """
    return "_QMLTYPE_" in className or className.endswith("_QML_")


# ---------------------------------------------------------------------------
# Tree walking
# ---------------------------------------------------------------------------

def iter_nodes(root: dict, ancestors: frozenset[int] = frozenset()):
    """Yield (node, ancestors) for every node in a snapshot tree.

    ``ancestors`` is the set of ancestor objIDs, used so a parent never
    counts as occluding its own descendants (children paint on top).
    """
    yield root, ancestors
    oid = root.get("objID")
    for child in root.get("children", []):
        new_anc = ancestors | ({oid} if oid is not None else set())
        yield from iter_nodes(child, new_anc)


def rebuild_tree(root: dict, keep: set[int], out_list: list[dict]) -> None:
    """Append the kept nodes of ``root``'s subtree to ``out_list``.

    A removed node is dropped entirely; its kept descendants are
    reparented one level up (they may still be visible even when their
    container is fully covered).
    """
    oid = root.get("objID")
    if oid in keep:
        out = dict(root)
        out.pop("children", None)
        child_out: list[dict] = []
        for k in root.get("children", []):
            rebuild_tree(k, keep, child_out)
        if child_out:
            out["children"] = child_out
        out_list.append(out)
    else:
        for k in root.get("children", []):
            rebuild_tree(k, keep, out_list)


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

    # ---- collect all elements in paint order --------------------------
    # Qt paints depth-first: a parent renders before its children, and
    # siblings render in ascending z (ties broken by declaration order).
    # So the paint sequence is a preorder walk where each node's children
    # are visited in (z, order) order.  Processing that sequence in
    # reverse (last-painted first) reproduces the real occlusion: a node
    # is covered by every opaque element painted after it, including its
    # own children and higher-z sibling subtrees -- but never by its
    # ancestors (they paint earlier; exclude below).
    #
    # z is only compared *between siblings*: a child (any z) always
    # paints above its parent, and a subtree paints above its parent's
    # lower-z siblings.  Sorting a flat (z, order) key across the whole
    # tree would be wrong -- a z=0 background inside a z=2 button would
    # sort before the button root and never cover it.
    #
    # QML exception: a child with negative z paints *under its parent's
    # content* (still inside the parent's subtree layer).  QtWidgets has
    # no such concept -- every child paints above its parent.  Negative-z
    # children therefore render before the parent (so the parent's rect
    # covers them during the reverse solve) -- handled by their exclude
    # set dropping the direct parent.
    elements = []  # (topLevelId, objID, rect, node, exclude)
    kept_ids: set[int] = set()  # no-geometry elements are kept as-is

    def visit(node: dict, ancestors: tuple) -> None:
        oid = node.get("objID")
        new_anc = ancestors if oid is None else ancestors + (oid,)
        children = node.get("children", [])

        def zkey(c: dict):
            return (float(c.get("z_order", 0) or 0), c.get("objID", 0))

        neg = [c for c in children if (float(c.get("z_order", 0) or 0)) < 0]
        pos = [c for c in children if (float(c.get("z_order", 0) or 0)) >= 0]
        # negative-z children paint before the parent
        for child in sorted(neg, key=zkey):
            visit(child, new_anc)
        if oid is not None and node.get("visible") is not False:
            rect = rect_from_node(node)
            if rect is not None:
                top = node.get("topLevelId", 0)
                # z < 0: painted under the parent's content, so the
                # direct parent (last ancestor) participates in covering
                # it; every higher ancestor still paints before it.
                exclude = frozenset(ancestors[:-1]) \
                    if (float(node.get("z_order", 0) or 0)) < 0 and ancestors \
                    else frozenset(ancestors)
                elements.append((top, oid, rect, node, exclude))
            else:
                kept_ids.add(oid)  # zero-size / missing geometry: keep
        # Hidden elements neither occlude nor are occluded; they are
        # dropped (their still-visible descendants are reparented up by
        # rebuild_tree).  A hidden element must never occlude the visible
        # UI underneath it (e.g. an invisible modal dialog), so hidden
        # nodes never enter `elements` -- but we still walk their
        # children for visible descendants.
        for child in sorted(pos, key=zkey):
            visit(child, new_anc)

    for root in nodes:
        visit(root, ())

    # ---- per window: accumulate coverage in reverse paint order -------
    # Walk the paint sequence backwards (last-painted first): each element
    # queries how much of it is still visible (everything painted after it
    # that is opaque), then registers itself as an occluder if opaque.
    by_window: dict[int, list] = {}
    for e in elements:
        by_window.setdefault(e[0], []).append(e)

    covered: dict[int, CoveredArea] = {}
    visible_ratio: dict[int, float] = {}
    for win, win_elems in by_window.items():
        cov = covered.setdefault(win, CoveredArea())
        for _top, oid, rect, node, exclude in reversed(win_elems):
            visible = cov.visible_area(rect, exclude=exclude)
            if visible < rect.area:
                ratio = visible / rect.area if rect.area > 0 else 0.0
                visible_ratio[oid] = ratio
            if is_occluder(node.get("className", "")):
                cov.insert(rect, source=oid)

    # ---- decide what to keep ------------------------------------------
    removed = 0
    for _top, oid, _rect, _node, _exclude in elements:
        ratio = visible_ratio.get(oid)
        if ratio is not None and ratio <= 0.0:
            removed += 1
            continue
        kept_ids.add(oid)

    # ---- rebuild the tree with annotations ----------------------------
    new_nodes: list[dict] = []
    for root in nodes:
        rebuild_tree(root, kept_ids, new_nodes)

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


