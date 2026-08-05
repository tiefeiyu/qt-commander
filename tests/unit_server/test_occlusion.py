"""Unit tests for snapshot occlusion pruning (qt_commander/occlusion.py)."""
import json

import pytest

from qt_commander.occlusion import (
    CoveredArea,
    Rect,
    is_occluder,
    prune_snapshot,
    rect_difference,
)


def node(oid, cls, x, y, w, h, z=0, top=1, children=None, visible=True):
    return {
        "objID": oid, "className": cls, "visible": visible, "z_order": z,
        "topLevelId": top,
        "global_rect": {"x": x, "y": y, "width": w, "height": h},
        "children": children or [],
    }


def snap(*nodes):
    return {"epoch": 1, "maxDepth": -1, "propDepth": 1, "rootId": 0,
            "nodes": list(nodes)}


def ids(nodes):
    out = []
    for n in nodes:
        out.append(n["objID"])
        out.extend(ids(n.get("children", [])))
    return out


# ---------------------------------------------------------------------------
# rect_difference
# ---------------------------------------------------------------------------

class TestRectDifference:
    def test_no_overlap_returns_original(self):
        r = rect_difference(Rect(0, 0, 10, 10), Rect(20, 20, 5, 5))
        assert len(r) == 1 and r[0] == Rect(0, 0, 10, 10)

    def test_full_overlap_returns_empty(self):
        assert rect_difference(Rect(0, 0, 10, 10), Rect(0, 0, 10, 10)) == []

    def test_center_cut_splits_four(self):
        pieces = rect_difference(Rect(0, 0, 10, 10), Rect(3, 3, 4, 4))
        assert sum(p.w * p.h for p in pieces) == 100 - 16

    def test_edge_cut_splits_two(self):
        pieces = rect_difference(Rect(0, 0, 10, 10), Rect(0, 0, 4, 4))
        assert sum(p.w * p.h for p in pieces) == 84


# ---------------------------------------------------------------------------
# CoveredArea
# ---------------------------------------------------------------------------

class TestCoveredArea:
    def test_insert_and_visible(self):
        ca = CoveredArea()
        ca.insert(Rect(0, 0, 10, 10))
        assert ca.visible_area(Rect(0, 0, 10, 10)) == 0
        assert ca.visible_area(Rect(10, 0, 10, 10)) == 100
        assert ca.visible_area(Rect(5, 5, 10, 10)) == 75

    def test_overlapping_inserts_union(self):
        ca = CoveredArea()
        ca.insert(Rect(0, 0, 10, 10))
        ca.insert(Rect(5, 5, 10, 10))
        # union = 100 + 100 - 25 overlap = 175 of 225
        assert ca.visible_area(Rect(0, 0, 15, 15)) == 50

    def test_disjoint_inserts(self):
        ca = CoveredArea()
        ca.insert(Rect(0, 0, 10, 10))
        ca.insert(Rect(20, 0, 10, 10))
        assert ca.visible_area(Rect(0, 0, 30, 10)) == 100

    def test_empty_bbox_fast_path(self):
        ca = CoveredArea()
        ca.insert(Rect(0, 0, 10, 10))
        assert ca.visible_area(Rect(100, 100, 10, 10)) == 100


# ---------------------------------------------------------------------------
# is_occluder heuristic
# ---------------------------------------------------------------------------

class TestIsOccluder:
    @pytest.mark.parametrize("cls,expected", [
        ("QWidget", True), ("QMainWindow", True), ("QPushButton", True),
        ("QLabel", True), ("QLineEdit", True),
        ("QQuickRectangle", True), ("QQuickImage", True),
        ("QQuickItem", False), ("QQuickText", False),
        ("QQuickLoader", False), ("QQuickMouseArea", False),
        ("QQuickRowLayout", False), ("QQuickListView", False),
        ("QGCButton_QMLTYPE_8", False),
        ("LoginDialog_QMLTYPE_761", False),
        ("MainRootWindow_QMLTYPE_676", False),
        ("", False),
    ])
    def test_heuristic(self, cls, expected):
        assert is_occluder(cls) is expected


# ---------------------------------------------------------------------------
# prune_snapshot end to end
# ---------------------------------------------------------------------------

class TestPruneSnapshot:
    def test_full_cover_removes_element(self):
        s = snap(
            node(1, "QWidget", 0, 0, 100, 100, z=0),
            node(2, "QWidget", 0, 0, 100, 100, z=1),
        )
        out = prune_snapshot(s)
        kept = ids(out["nodes"])
        assert 2 in kept and 1 not in kept
        assert out["pruned"]["removed"] == 1

    def test_partial_cover_keeps_with_ratio(self):
        # partially covered elements survive with a visible_ratio field
        s = snap(
            node(1, "QWidget", 0, 0, 100, 100, z=0),
            node(2, "QWidget", 0, 0, 50, 100, z=1),
        )
        out = prune_snapshot(s)
        kept = ids(out["nodes"])
        assert 1 in kept and 2 in kept
        assert out["nodes"][0]["visible_ratio"] == pytest.approx(0.5)
        assert "visible_ratio" not in out["nodes"][1]

    def test_same_z_later_created_occludes_earlier(self):
        # Qt paints equal-z siblings in tree order: the later-created
        # element (later in the children array) covers the earlier one.
        s = snap(
            node(1, "QWidget", 0, 0, 100, 100, z=1),
            node(2, "QWidget", 0, 0, 100, 100, z=1),
        )
        out = prune_snapshot(s)
        kept = ids(out["nodes"])
        assert 2 in kept and 1 not in kept

    def test_same_z_partial_overlap(self):
        s = snap(
            node(1, "QWidget", 0, 0, 100, 100, z=1),
            node(2, "QWidget", 0, 0, 50, 100, z=1),
        )
        out = prune_snapshot(s)
        kept = ids(out["nodes"])
        assert 1 in kept and 2 in kept
        assert out["nodes"][0]["visible_ratio"] == pytest.approx(0.5)

    def test_same_z_nested_tree_order(self):
        # Tree order is global (DFS): at equal z a later top-level
        # sibling covers the whole subtree of an earlier one.
        s = snap(node(1, "QWidget", 0, 0, 100, 100, z=1, children=[
            node(3, "QWidget", 0, 0, 100, 100, z=1),
            node(4, "QWidget", 0, 0, 100, 100, z=1),
        ]), node(2, "QWidget", 0, 0, 100, 100, z=1))
        out = prune_snapshot(s)
        kept = ids(out["nodes"])
        assert kept == [2]

    def test_transparent_does_not_occlude(self):
        s = snap(
            node(1, "QWidget", 0, 0, 100, 100, z=0),
            node(2, "QQuickItem", 0, 0, 100, 100, z=1),
            node(3, "QGCButton_QMLTYPE_8", 0, 0, 100, 100, z=1),
        )
        out = prune_snapshot(s)
        assert sorted(ids(out["nodes"])) == [1, 2, 3]

    def test_qml_rectangle_occludes(self):
        s = snap(
            node(1, "QWidget", 0, 0, 100, 100, z=0),
            node(2, "QQuickRectangle", 0, 0, 100, 100, z=1),
        )
        out = prune_snapshot(s)
        assert 2 in ids(out["nodes"]) and 1 not in ids(out["nodes"])

    def test_cross_window_no_occlusion(self):
        s = snap(
            node(1, "QWidget", 0, 0, 100, 100, z=0, top=1),
            node(2, "QWidget", 0, 0, 100, 100, z=1, top=2),
        )
        out = prune_snapshot(s)
        assert sorted(ids(out["nodes"])) == [1, 2]

    def test_z_order_sorting(self):
        # z=0 bottom, z=2 top: bottom fully covered by top
        s = snap(
            node(1, "QWidget", 0, 0, 100, 100, z=0),
            node(2, "QWidget", 0, 0, 100, 100, z=2),
        )
        out = prune_snapshot(s)
        assert 2 in ids(out["nodes"]) and 1 not in ids(out["nodes"])

    def test_nested_tree_removes_covered_child(self):
        s = snap(node(1, "QWidget", 0, 0, 100, 100, z=0, children=[
            node(3, "QWidget", 0, 0, 100, 100, z=0),
        ]), node(2, "QWidget", 0, 0, 100, 100, z=1))
        out = prune_snapshot(s)
        kept = ids(out["nodes"])
        assert 2 in kept and 1 not in kept and 3 not in kept

    def test_container_with_visible_children_kept(self):
        # 2 covers only the top half; child 3 in the bottom half stays
        # visible, and container 1 survives partially covered.
        s = snap(node(1, "QWidget", 0, 0, 100, 100, z=0, children=[
            node(3, "QWidget", 50, 50, 10, 10, z=0),
        ]), node(2, "QWidget", 0, 0, 100, 50, z=1))
        out = prune_snapshot(s)
        kept = ids(out["nodes"])
        assert 3 in kept and 2 in kept and 1 in kept
        # 1 is covered by 2 (top half) and by its own child 3 (10x10)
        assert out["nodes"][0]["visible_ratio"] == pytest.approx(0.49)

    def test_fully_covered_child_removed(self):
        # 2 covers the whole container incl. its child
        s = snap(node(1, "QWidget", 0, 0, 100, 100, z=0, children=[
            node(3, "QWidget", 50, 50, 10, 10, z=0),
        ]), node(2, "QWidget", 0, 0, 100, 100, z=1))
        out = prune_snapshot(s)
        kept = ids(out["nodes"])
        assert kept == [2]

    def test_parent_never_occludes_own_child(self):
        # children always paint above their parent: a fully covered
        # container still keeps its visible children
        s = snap(node(1, "QWidget", 0, 0, 100, 100, z=0, children=[
            node(3, "QWidget", 0, 0, 100, 100, z=0),
        ]), node(2, "QWidget", 0, 0, 100, 100, z=1))
        out = prune_snapshot(s)
        kept = ids(out["nodes"])
        assert 2 in kept and 3 not in kept  # 3 is covered by 2, not by 1

    def test_child_paints_over_parent(self):
        # a small child only covers its own area of the parent
        s = snap(node(1, "QWidget", 0, 0, 100, 100, z=0, children=[
            node(3, "QWidget", 50, 50, 10, 10, z=0),
        ]))
        out = prune_snapshot(s)
        kept = ids(out["nodes"])
        assert 3 in kept and 1 in kept
        assert out["nodes"][0]["visible_ratio"] == pytest.approx(0.99)

    def test_fullsize_child_covers_parent(self):
        # an opaque full-size child paints over the whole parent
        s = snap(node(1, "QWidget", 0, 0, 100, 100, z=0, children=[
            node(3, "QWidget", 0, 0, 100, 100, z=0),
        ]))
        out = prune_snapshot(s)
        kept = ids(out["nodes"])
        assert kept == [3]

    def test_empty_snapshot(self):
        out = prune_snapshot(snap())
        assert out["nodes"] == []
        assert out["pruned"]["removed"] == 0

    def test_zero_size_elements_ignored(self):
        s = snap(node(1, "QWidget", 0, 0, 0, 0, z=0))
        out = prune_snapshot(s)
        assert ids(out["nodes"]) == [1]

    def test_no_overlap_keeps_everything(self):
        s = snap(
            node(1, "QWidget", 0, 0, 10, 10),
            node(2, "QWidget", 50, 50, 10, 10),
        )
        out = prune_snapshot(s)
        assert sorted(ids(out["nodes"])) == [1, 2]
        assert out["pruned"]["removed"] == 0

    def test_missing_geometry_kept(self):
        s = {"epoch": 1, "maxDepth": -1, "propDepth": 1, "rootId": 0,
             "nodes": [{"objID": 1, "className": "QWidget", "visible": True,
                        "z_order": 0, "topLevelId": 1, "children": []}]}
        out = prune_snapshot(s)
        assert ids(out["nodes"]) == [1]

    def test_qml_component_root_covered_by_own_background(self):
        # A custom component root (button) covered by its own full-size
        # background child is fully hidden: the root is removed and the
        # visible background child is reparented up.
        s = snap(node(1, "QGCButton_QMLTYPE_8", 0, 0, 100, 100, z=0, children=[
            node(3, "QQuickRectangle", 0, 0, 100, 100, z=0),
        ]))
        out = prune_snapshot(s)
        kept = ids(out["nodes"])
        assert 3 in kept and 1 not in kept

    def test_qml_component_root_removed_when_covered_externally(self):
        # ...but a component covered by a *sibling* opaque element is
        # still removed (it is genuinely hidden).
        s = snap(
            node(1, "QGCButton_QMLTYPE_8", 0, 0, 100, 100, z=0),
            node(2, "QWidget", 0, 0, 100, 100, z=1),
        )
        out = prune_snapshot(s)
        kept = ids(out["nodes"])
        assert kept == [2]

    def test_qml_component_kept_when_partially_visible(self):
        s = snap(
            node(1, "QGCButton_QMLTYPE_8", 0, 0, 100, 100, z=0),
            node(2, "QWidget", 0, 0, 100, 50, z=1),
        )
        out = prune_snapshot(s)
        kept = ids(out["nodes"])
        assert 1 in kept and 2 in kept

    def test_roundtrip_json_serializable(self):
        s = snap(node(1, "QWidget", 0, 0, 100, 100, z=0),
                 node(2, "QWidget", 0, 0, 50, 100, z=1))
        out = prune_snapshot(s)
        json.dumps(out)  # must not raise
