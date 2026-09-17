#!/usr/bin/env python3
"""
mapdelta -- author, apply and inspect HPL2 .map_delta / .ent_delta patches.

A delta records only what you changed: which objects you added, removed or
edited. It contains no base-game map data, which is what makes it the file you
can redistribute -- the player's own copy of the game supplies the base .map,
and the engine merges the two in memory at load time.

The usual loop, against a working copy of the game:

    mapdelta.py apply <work_dir> <delta_dir> --in-place
    #  ... open <work_dir>/maps/.../whatever.map in the LevelEditor, edit, save
    mapdelta.py diff  <game_dir> <work_dir> --out <delta_dir>

--in-place overwrites the maps in <work_dir> outright, so point it at a copy and
keep <game_dir> pristine -- 'diff' needs an unpatched base to compare against.
Pass --backup to stash each original as <file>.mapdelta-orig instead; later runs
then read the stash as the base, which makes re-running --in-place idempotent and
lets 'diff <dir> <dir>' work against a single folder.

To leave the target folder alone entirely, write the patched files elsewhere:

    mapdelta.py apply <game_dir> <delta_dir> --out work/

'diff' walks the modified tree, so work/ only ever needs to hold the maps you
actually touched. Delta files mirror the relative path of the file they patch:
maps/ptest/01_cells.map -> <delta_dir>/maps/ptest/01_cells.map_delta

This is the same format and the same operation semantics the engine implements
in HPL2/core/sources/resources/XmlDelta.cpp. Keep the two in step.
"""

import argparse
import copy
import os
import shutil
import sys
import xml.etree.ElementTree as ET

# --------------------------------------------------------------------------
# Format constants -- must match HPL2/core/include/resources/XmlDelta.h
# --------------------------------------------------------------------------

FORMAT_VERSION = 1

# First ID handed to an <Add>ed object. Well above anything the editor's dense
# counter produces, so added objects never collide with base IDs (save games
# persist entity state against those IDs).
FIRST_ADD_ID = 1000000

# Object categories under <MapContents>. "Misc" (<Compound>) is omitted because
# cWorldLoaderHplMap never reads it; "StaticObjectCombos" is derived data keyed
# on static object IDs and is maintained by the applier, not diffed.
MAP_CATEGORIES = ["StaticObjects", "Primitives", "Decals", "Entities"]

# Object categories under <ModelData> in a .ent file. "Entities" holds the
# attached lights, billboards, particle systems and sounds -- present in 903 of
# the 909 shipped .ent files, and where most entity edits actually land.
ENT_CATEGORIES = ["Mesh", "Bones", "Shapes", "Bodies", "Joints", "Animations", "Entities"]

# Attributes with no meaning to the game loader, or that index into per-file
# tables a delta must not depend on. Excluded from comparison, stripped from
# added objects.
IGNORED_ATTRIBUTES = {"Group", "GUID", "FileIndex", "MaterialIndex"}

# The two spellings of the per-object variable block.
USER_VAR_TAGS = ("UserVariables", "UserDefinedVariables")

# Float tolerance. The engine writes floats with "%g" -- 6 significant digits --
# so the representation error grows with magnitude: ~5e-4 at |v|=100, ~5e-3 at
# |v|=1000. A purely absolute epsilon therefore reports phantom edits on the
# large world coordinates real maps use. Scale it, keeping an absolute floor for
# values near zero. The relative term stays well above the format's own noise
# floor (~5e-6) and well below any edit a human would make.
FLOAT_EPSILON = 1e-4
FLOAT_EPSILON_RELATIVE = 1e-5

# How each category's file index resolves to a literal path attribute. Mirrors
# the fallbacks in cWorldLoaderHplMap: an object whose index is absent or
# negative is loaded from these attributes instead.
FILE_INDEX_RULES = {
    "StaticObjects": ("FileIndex", "FileIndex_StaticObjects", "MeshFilename"),
    "Entities":      ("FileIndex", "FileIndex_Entities",      "Filename"),
    "Decals":        ("MaterialIndex", "FileIndex_Decals",    "Material"),
}


class DeltaError(Exception):
    pass


# --------------------------------------------------------------------------
# XML helpers
# --------------------------------------------------------------------------

def parse_xml(path):
    try:
        return ET.parse(path)
    except ET.ParseError as e:
        raise DeltaError("could not parse '%s': %s" % (path, e))


def indent(elem, level=0):
    """In-place pretty printer -- ElementTree has no equivalent before 3.9."""
    pad = "\n" + "    " * level
    if len(elem):
        if not elem.text or not elem.text.strip():
            elem.text = pad + "    "
        for child in elem:
            indent(child, level + 1)
        if not child.tail or not child.tail.strip():
            child.tail = pad
    if level and (not elem.tail or not elem.tail.strip()):
        elem.tail = pad


def write_xml(root, path):
    indent(root)
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    ET.ElementTree(root).write(path, encoding="utf-8", xml_declaration=False)


def values_equal(a, b):
    """
    Attribute values round-trip through the editor's float formatting, so "0"
    and "1e-011" and "1.00001" must not read as edits. Compare numerically when
    both sides are numeric tuples of the same arity, textually otherwise.
    """
    if a == b:
        return True

    # Asset paths compare by their root-relative form.
    if "/" in a or "\\" in a or "/" in b or "\\" in b:
        return normalize_asset_path(a) == normalize_asset_path(b)

    ta, tb = a.split(), b.split()
    if not ta or len(ta) != len(tb):
        return False

    try:
        for x, y in zip(ta, tb):
            fx, fy = float(x), float(y)
            tolerance = max(FLOAT_EPSILON, FLOAT_EPSILON_RELATIVE * max(abs(fx), abs(fy)))
            if abs(fx - fy) > tolerance:
                return False
        return True
    except ValueError:
        return False


def normalize_asset_path(value):
    """
    Collapse an asset path to its root-relative form.

    The editor sometimes writes a path relative to the wrong root, producing
    long "../../.." prefixes (66 levels deep in real maps). The engine resolves
    assets by bare filename through cFileSearcher, so these still load and the
    corruption goes unnoticed -- but it is the same asset, and a delta must
    neither report it as a change nor propagate it.
    """
    if "/" not in value and "\\" not in value:
        return value

    parts = [p for p in value.replace("\\", "/").split("/") if p not in ("", ".", "..")]
    return "/".join(parts)


def find_user_vars(elem):
    for tag in USER_VAR_TAGS:
        found = elem.find(tag)
        if found is not None:
            return found
    return None


def get_user_vars(elem):
    block = find_user_vars(elem)
    if block is None:
        return {}
    return {v.get("Name"): v.get("Value", "") for v in block.findall("Var") if v.get("Name")}


# --------------------------------------------------------------------------
# One side of a diff / the target of an apply
# --------------------------------------------------------------------------

class Document:
    """A parsed .map or .ent, plus the lookups both diff and apply need."""

    def __init__(self, path):
        self.path = path
        self.tree = parse_xml(path)
        self.root = self.tree.getroot()

        data = self.root.find("MapData")
        if data is not None:
            self.kind = "map"
            self.data = data
            self.contents = data.find("MapContents")
            self.categories = MAP_CATEGORIES
        else:
            data = self.root.find("ModelData")
            if data is None:
                raise DeltaError("'%s' has neither a <MapData> nor a <ModelData> element" % path)
            self.kind = "ent"
            self.data = self.root
            self.contents = data
            self.categories = ENT_CATEGORIES

        if self.contents is None:
            raise DeltaError("'%s' has no contents element" % path)

        self.file_indices = {}
        for _, index_tag, _ in FILE_INDEX_RULES.values():
            self.file_indices[index_tag] = self._load_file_index(index_tag)

    def _load_file_index(self, tag):
        elem = self.contents.find(tag)
        if elem is None:
            return {}
        table = {}
        for f in elem:
            try:
                table[int(f.get("Id", "-1"))] = f.get("Path", "")
            except ValueError:
                continue
        return table

    # ----------------------------------------------------------------

    def objects(self, category):
        """Objects of one category, keyed by ID."""
        parent = self.contents.find(category)
        if parent is None:
            return {}

        out = {}
        for obj in parent:
            raw = obj.get("ID")
            if raw is None:
                continue
            try:
                obj_id = int(raw)
            except ValueError:
                continue

            # IDs are the match key, so a duplicate would make every operation
            # against it ambiguous. The editor does not produce these; a
            # hand-merged file can.
            if obj_id in out:
                warn("%s: duplicate %s ID %d ('%s' and '%s') -- only the last is used"
                     % (self.path, category, obj_id,
                        out[obj_id].get("Name", "?"), obj.get("Name", "?")))
            out[obj_id] = obj
        return out

    def category_element(self, category, create=False):
        parent = self.contents.find(category)
        if parent is None and create:
            parent = ET.SubElement(self.contents, category)
        return parent

    def resolve_file_path(self, obj, category):
        """
        The literal-path attribute an object's file index stands for.
        Returns (attribute_name, path) or None.
        """
        rule = FILE_INDEX_RULES.get(category)
        if rule is None:
            return None

        index_attr, index_tag, path_attr = rule
        raw = obj.get(index_attr)
        if raw is None:
            return None

        try:
            index = int(raw)
        except ValueError:
            return None
        if index < 0:
            return None

        path = self.file_indices[index_tag].get(index)
        if path is None:
            warn("'%s' has %s %d, out of range of %s"
                 % (obj.get("Name", "?"), index_attr, index, index_tag))
            return None

        return path_attr, path

    def normalized_attributes(self, obj, category):
        """Comparable attribute set: indices replaced by the path they resolve
        to, editor bookkeeping dropped."""
        attrs = {k: v for k, v in obj.attrib.items() if k not in IGNORED_ATTRIBUTES}

        resolved = self.resolve_file_path(obj, category)
        if resolved:
            attrs[resolved[0]] = resolved[1]

        return attrs

    def all_names(self):
        return set(self.objects_by_name())

    def objects_by_name(self):
        names = {}
        for category in self.contents:
            for obj in category:
                name = obj.get("Name")
                if name:
                    names.setdefault(name.lower(), []).append(obj)
        return names

    def save(self, path):
        write_xml(self.root, path)


def warn(message):
    print("WARNING: %s" % message, file=sys.stderr)


# --------------------------------------------------------------------------
# Diff
# --------------------------------------------------------------------------

def _copy_added_object(obj, category, modified):
    """
    Copy an object out of the modified document into an <Add> block: strip the
    map's own bookkeeping and swap the file index for the literal path the
    loader accepts, so the added object does not depend on the base map's index
    tables (the editor renumbers them on every save).
    """
    result = ET.Element(obj.tag)
    for name, value in obj.attrib.items():
        # ID is assigned by the applier, from a range that cannot collide.
        if name == "ID" or name in IGNORED_ATTRIBUTES:
            continue
        result.set(name, normalize_asset_path(value))

    resolved = modified.resolve_file_path(obj, category)
    if resolved:
        result.set(resolved[0], resolved[1])

    # Carry the variable block across verbatim. <DecalMesh> is deliberately not
    # copied: it is baked geometry the loader rebuilds from the transform.
    block = find_user_vars(obj)
    if block is not None:
        result.append(copy.deepcopy(block))

    return result


def _diff_category(category, base, modified, delta_root, witness):
    base_objects = base.objects(category)
    modified_objects = modified.objects(category)
    ops = 0

    # Objects the modification deleted
    for obj_id in sorted(set(base_objects) - set(modified_objects)):
        op = ET.SubElement(delta_root, "Remove")
        op.set("Category", category)
        op.set("ID", str(obj_id))
        if witness:
            op.set("Name", base_objects[obj_id].get("Name", ""))
        ops += 1

    # Objects that survived, with changed attributes or variables
    for obj_id in sorted(set(base_objects) & set(modified_objects)):
        old, new = base_objects[obj_id], modified_objects[obj_id]

        old_attrs = base.normalized_attributes(old, category)
        new_attrs = modified.normalized_attributes(new, category)
        changed = {k: v for k, v in new_attrs.items()
                   if k not in old_attrs or not values_equal(old_attrs[k], v)}
        # Attributes the modified file no longer has. ID is the match key and is
        # never removed. Ignored attributes never reach either map, so an editor
        # dropping a GUID or a FileIndex cannot produce a RemoveAttr.
        removed_attrs = sorted(k for k in old_attrs if k not in new_attrs and k != "ID")

        old_vars, new_vars = get_user_vars(old), get_user_vars(new)
        changed_vars = {k: v for k, v in new_vars.items()
                        if k not in old_vars or not values_equal(old_vars[k], v)}
        removed_vars = sorted(set(old_vars) - set(new_vars))

        if not changed and not removed_attrs and not changed_vars and not removed_vars:
            continue

        op = ET.SubElement(delta_root, "Modify")
        op.set("Category", category)
        op.set("ID", str(obj_id))
        if witness:
            op.set("Name", old.get("Name", ""))

        if changed:
            set_attr = ET.SubElement(op, "SetAttr")
            for name in sorted(changed):
                set_attr.set(name, normalize_asset_path(changed[name]))

        for name in removed_attrs:
            ET.SubElement(op, "RemoveAttr", {"Name": name})

        for name in sorted(changed_vars):
            ET.SubElement(op, "SetVar", {"Name": name, "Value": changed_vars[name]})
        for name in removed_vars:
            ET.SubElement(op, "RemoveVar", {"Name": name})

        ops += 1

    # Objects the modification introduced
    added = sorted(set(modified_objects) - set(base_objects))
    if added:
        add = ET.SubElement(delta_root, "Add")
        add.set("Category", category)
        for obj_id in added:
            add.append(_copy_added_object(modified_objects[obj_id], category, modified))
            ops += 1

    return ops


def _diff_root_data(base, modified, delta_root):
    """Attribute overrides on <MapData> (fog, skybox, ...)."""
    op = None
    for name, value in sorted(modified.data.attrib.items()):
        old = base.data.get(name)
        if old is not None and values_equal(old, value):
            continue
        if op is None:
            op = ET.SubElement(delta_root, "SetMapData")
        op.set(name, value)

    return 1 if op is not None else 0


def _diff_root_vars(base, modified, delta_root):
    """An .ent's <UserDefinedVariables> hang off the root, not off an object."""
    # The block's own attributes (EntityType / EntitySubType) select which engine
    # loader builds the entity. The format has no operation for them, so say so
    # rather than dropping the change on the floor.
    old_block, new_block = find_user_vars(base.data), find_user_vars(modified.data)
    if old_block is not None and new_block is not None:
        if dict(old_block.attrib) != dict(new_block.attrib):
            warn("%s: <%s> attributes changed (%s -> %s); a delta cannot express "
                 "this, ship the .ent itself"
                 % (modified.path, new_block.tag,
                    dict(old_block.attrib), dict(new_block.attrib)))

    old_vars, new_vars = get_user_vars(base.data), get_user_vars(modified.data)
    ops = 0

    for name in sorted(new_vars):
        if name not in old_vars or not values_equal(old_vars[name], new_vars[name]):
            ET.SubElement(delta_root, "SetVar", {"Name": name, "Value": new_vars[name]})
            ops += 1

    for name in sorted(set(old_vars) - set(new_vars)):
        ET.SubElement(delta_root, "RemoveVar", {"Name": name})
        ops += 1

    return ops


def diff_file(base_path, modified_path, name=None, priority=0, target=None, witness=True):
    """Returns (delta_root, op_count). op_count 0 means the files are equivalent."""
    base = Document(base_path)
    modified = Document(modified_path)

    if base.kind != modified.kind:
        raise DeltaError("'%s' and '%s' are different kinds of document"
                         % (base_path, modified_path))

    root = ET.Element("MapDelta" if base.kind == "map" else "EntDelta")
    root.set("Version", str(FORMAT_VERSION))
    root.set("Target", target or os.path.basename(base_path))
    if name:
        root.set("Name", name)
    if priority:
        root.set("Priority", str(priority))

    ops = 0
    if base.kind == "map":
        ops += _diff_root_data(base, modified, root)
    else:
        ops += _diff_root_vars(base, modified, root)

    for category in base.categories:
        ops += _diff_category(category, base, modified, root, witness)

    return root, ops


# --------------------------------------------------------------------------
# Apply -- mirrors ApplyXmlDelta in HPL2/core/sources/resources/XmlDelta.cpp
# --------------------------------------------------------------------------

class Stats:
    def __init__(self):
        self.added = self.modified = self.removed = self.skipped = 0

    def add(self, other):
        self.added += other.added
        self.modified += other.modified
        self.removed += other.removed
        self.skipped += other.skipped

    def __str__(self):
        return "%d added, %d modified, %d removed, %d skipped" % (
            self.added, self.modified, self.removed, self.skipped)


def _resolve_op_target(document, op, op_name, objects_cache):
    """
    Resolve the object an op points at and check the optional Name / GUID
    witnesses. A mismatch means the base file has changed under the delta, so
    the op is refused rather than applied to the wrong object.
    """
    category = op.get("Category")
    raw_id = op.get("ID")
    if not category or raw_id is None:
        warn("<%s> needs both a Category and an ID" % op_name)
        return None, None, None

    try:
        obj_id = int(raw_id)
    except ValueError:
        warn("<%s> has a non-numeric ID '%s'" % (op_name, raw_id))
        return None, None, None

    if category not in objects_cache:
        objects_cache[category] = document.objects(category)
    obj = objects_cache[category].get(obj_id)

    if obj is None:
        warn("<%s> target %s ID %d does not exist in the base file" % (op_name, category, obj_id))
        return None, None, None

    wanted = op.get("Name")
    if wanted:
        actual = obj.get("Name", "")
        if actual.lower() != wanted.lower():
            warn("<%s> target %s ID %d is named '%s', delta expected '%s'. Skipping."
                 % (op_name, category, obj_id, actual, wanted))
            return None, None, None

    wanted = op.get("GUID")
    if wanted:
        actual = obj.get("GUID", "")
        if actual and actual.lower() != wanted.lower():
            warn("<%s> target %s ID %d has GUID '%s', delta expected '%s'. Skipping."
                 % (op_name, category, obj_id, actual, wanted))
            return None, None, None

    return category, obj_id, obj


def _remove_id_from_combos(document, obj_id):
    """A removed static object must also leave any combine group it was part of,
    or cWorldLoaderHplMap warns once per missing id."""
    combos = document.contents.find("StaticObjectCombos")
    if combos is None:
        return

    for combo in combos:
        raw = combo.get("ObjIds")
        if not raw:
            continue
        kept = [t for t in raw.split() if t != str(obj_id)]
        if len(kept) != len(raw.split()):
            combo.set("ObjIds", " ".join(kept))


def _apply_var_ops(target, op):
    for sub in op:
        if sub.tag == "SetVar":
            name = sub.get("Name")
            if not name:
                warn("<SetVar> without a Name")
                continue

            block = find_user_vars(target)
            if block is None:
                block = ET.SubElement(target, USER_VAR_TAGS[0])

            var = next((v for v in block.findall("Var") if v.get("Name") == name), None)
            if var is None:
                var = ET.SubElement(block, "Var", {"Name": name})
            var.set("Value", sub.get("Value", ""))

        elif sub.tag == "RemoveVar":
            name = sub.get("Name")
            block = find_user_vars(target)
            if not name or block is None:
                continue
            var = next((v for v in block.findall("Var") if v.get("Name") == name), None)
            if var is not None:
                block.remove(var)


# The area light is the one shape with no Standard class. Every other light is a
# single element carrying both backends' values, so it defaults to both.
RAYTRACED_LIGHT_TAGS = ("AreaLight",)


def renderer_mask(obj):
    """Renderers an object loads for (1 Standard, 2 RayTraced), as XmlDelta.cpp
    reads it: RendererMask, else 2 for the ray-traced-only area light, else both."""
    if obj.get("RendererMask") is not None:
        try:
            return int(obj.get("RendererMask")) & 3
        except ValueError:
            return 0
    return 2 if obj.tag in RAYTRACED_LIGHT_TAGS else 3


def fits_base_name_count(same, obj, base_count, extra):
    allowed = max(base_count, 1)
    mask = renderer_mask(obj)
    return all(extra + sum(1 for other in same if renderer_mask(other) & bit) <= allowed
               for bit in (1, 2) if mask & bit)


def apply_delta(document, delta_root, next_add_id):
    """Applies one delta to a parsed Document, in place.
    Returns (stats, next_add_id)."""
    stats = Stats()

    try:
        version = int(delta_root.get("Version", FORMAT_VERSION))
    except ValueError:
        version = FORMAT_VERSION
    if version > FORMAT_VERSION:
        raise DeltaError("delta has format version %d, this tool understands %d"
                         % (version, FORMAT_VERSION))

    objects_cache = {}
    names_in_use = document.objects_by_name()
    base_name_counts = {name: len(objs) for name, objs in names_in_use.items()}
    added_objects = []

    for op in delta_root:
        tag = op.tag

        if tag == "SetMapData":
            document.data.attrib.update(op.attrib)
            stats.modified += 1

        elif tag in ("SetVar", "RemoveVar"):
            wrapper = ET.Element("Modify")
            wrapper.append(copy.deepcopy(op))
            _apply_var_ops(document.data, wrapper)
            stats.modified += 1

        elif tag == "Remove":
            category, obj_id, obj = _resolve_op_target(document, op, "Remove", objects_cache)
            if obj is None:
                stats.skipped += 1
                continue

            if category == "StaticObjects":
                _remove_id_from_combos(document, obj_id)

            document.category_element(category).remove(obj)
            del objects_cache[category][obj_id]
            same = names_in_use.get(obj.get("Name", "").lower(), [])
            if obj in same:
                same.remove(obj)
            stats.removed += 1

        elif tag == "Modify":
            category, obj_id, obj = _resolve_op_target(document, op, "Modify", objects_cache)
            if obj is None:
                stats.skipped += 1
                continue

            for set_attr in op.findall("SetAttr"):
                for name, value in set_attr.attrib.items():
                    if name != "ID":
                        obj.set(name, value)

                # Every loader prefers an object's file index over its literal
                # path attribute whenever the index is present, so repointing an
                # object at a different mesh/entity/material only takes effect
                # once the competing index is gone.
                if set_attr.get("Filename") is not None or set_attr.get("MeshFilename") is not None:
                    obj.attrib.pop("FileIndex", None)
                if set_attr.get("Material") is not None:
                    obj.attrib.pop("MaterialIndex", None)

            for remove_attr in op.findall("RemoveAttr"):
                name = remove_attr.get("Name")
                if not name or name == "ID":
                    warn("<RemoveAttr> needs a Name, and cannot remove ID")
                    continue
                obj.attrib.pop(name, None)

            _apply_var_ops(obj, op)
            stats.modified += 1

        elif tag == "Add":
            category = op.get("Category")
            if not category:
                warn("<Add> without a Category")
                stats.skipped += 1
                continue

            parent = document.category_element(category, create=True)

            # After="ID" inserts the added objects, in order, behind that object.
            anchor = None
            if op.get("After") is not None:
                anchor = next((o for o in parent if o.get("ID") == op.get("After")), None)
                if anchor is None:
                    warn("<Add After=\"%s\"> names no object in %s. Appending instead." % (op.get("After"), category))

            for new in op:
                # FileIndex/MaterialIndex refer to the base file's index tables,
                # which a delta must not renumber.
                if new.get("FileIndex") is not None or new.get("MaterialIndex") is not None:
                    warn("added '%s' uses FileIndex/MaterialIndex; use a literal "
                         "Filename/MeshFilename/Material instead. Skipping."
                         % new.get("Name", "?"))
                    stats.skipped += 1
                    continue

                # A name may be reused only by an object with an explicit
                # RendererMask that leaves no renderer with the name more often
                # than the base file had it (one half of a backend split).
                name = new.get("Name", "")
                same = names_in_use.get(name.lower(), []) if name else []
                if same and not (new.get("RendererMask") is not None and renderer_mask(new)
                                 and fits_base_name_count(same, new, base_name_counts.get(name.lower(), 0), 1)):
                    warn("added object name '%s' is already used in the base file. Skipping." % name)
                    stats.skipped += 1
                    continue

                added = copy.deepcopy(new)
                added.set("ID", str(next_add_id))
                next_add_id += 1

                if anchor is not None:
                    parent.insert(list(parent).index(anchor) + 1, added)
                    anchor = added
                else:
                    parent.append(added)
                if category in objects_cache:
                    objects_cache[category][int(added.get("ID"))] = added
                if name:
                    names_in_use.setdefault(name.lower(), []).append(added)
                added_objects.append((parent, added))
                stats.added += 1

        else:
            warn("unknown operation <%s>" % tag)
            stats.skipped += 1

    # A later <Modify> can widen a mask after the <Add> was accepted.
    for parent, added in added_objects:
        name = added.get("Name", "").lower()
        same = names_in_use.get(name, [])
        if name and added in same and not fits_base_name_count(same, added, base_name_counts.get(name, 0), 0):
            warn("added object '%s' shares its name with an object loaded for the same renderer. Dropping it."
                 % added.get("Name"))
            parent.remove(added)
            same.remove(added)
            stats.added -= 1
            stats.skipped += 1

    return stats, next_add_id


def apply_files(base_path, delta_paths, out_path):
    document = Document(base_path)
    next_add_id = FIRST_ADD_ID
    total = Stats()

    for delta_path in delta_paths:
        delta_root = parse_xml(delta_path).getroot()
        stats, next_add_id = apply_delta(document, delta_root, next_add_id)
        total.add(stats)

    document.save(out_path)
    return total


# --------------------------------------------------------------------------
# Tree walking
# --------------------------------------------------------------------------

PATCHABLE = {".map": "map_delta", ".ent": "ent_delta"}


def walk_patchable(root_dir):
    """{path relative to root_dir: absolute path} for every file a delta can
    describe. Backup stashes are skipped -- their extension is not patchable."""
    found = {}

    for dirpath, _, filenames in os.walk(root_dir):
        for filename in sorted(filenames):
            ext = os.path.splitext(filename)[1].lower()
            if ext not in PATCHABLE:
                continue

            full = os.path.join(dirpath, filename)
            found[os.path.relpath(full, root_dir)] = full

    return found


def delta_path_for(relative_path, delta_dir):
    base, ext = os.path.splitext(relative_path)
    return os.path.join(delta_dir, base + "." + PATCHABLE[ext.lower()])


# Suffix of the pristine copy `apply --in-place --backup` stashes beside a file
# before overwriting it. It is the base every later diff and re-apply reads from,
# which is what keeps in-place patching repeatable.
BACKUP_SUFFIX = ".mapdelta-orig"


def backup_path_for(path):
    return path + BACKUP_SUFFIX


def pristine_path_for(path):
    """The unpatched version of a file: its stashed original if one exists,
    otherwise the file itself."""
    backup = backup_path_for(path)
    return backup if os.path.isfile(backup) else path


def source_path_for(delta_relative_path):
    """maps/x/y.map_delta -> maps/x/y.map"""
    base, ext = os.path.splitext(delta_relative_path)
    for source_ext, delta_ext in PATCHABLE.items():
        if ext == "." + delta_ext:
            return base + source_ext
    return None


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------

def command_diff(args):
    written, skipped, unchanged = [], [], 0
    only_modified, only_base = [], []

    # Walk both roots and work from the union of what they hold, so a file added
    # or deleted on one side is reported rather than silently passed over.
    base_files = walk_patchable(args.base_dir)
    modified_files = walk_patchable(args.modified_dir)

    for relative in sorted(set(base_files) | set(modified_files)):
        if relative not in modified_files:
            only_base.append(relative)
            continue
        if relative not in base_files:
            only_modified.append(relative)
            continue

        modified_path = modified_files[relative]

        # If the base folder was patched with `apply --in-place --backup`, the
        # unpatched file is the stash beside it -- diffing against the patched
        # one would quietly produce an empty delta.
        base_path = pristine_path_for(base_files[relative])

        try:
            root, ops = diff_file(base_path, modified_path,
                                  name=args.name, priority=args.priority,
                                  target=relative.replace(os.sep, "/"),
                                  witness=not args.no_witness)
        except DeltaError as e:
            skipped.append((relative, str(e)))
            continue

        out_path = delta_path_for(relative, args.out)
        if ops == 0:
            unchanged += 1
            # A previously generated delta that no longer describes a change is
            # stale: leaving it behind would silently re-apply old edits.
            if os.path.isfile(out_path):
                if args.prune:
                    os.remove(out_path)
                    print("  removed stale %s" % os.path.relpath(out_path, args.out))
                else:
                    warn("%s describes no changes any more but still exists "
                         "(pass --prune to delete it)" % os.path.relpath(out_path, args.out))
            continue

        write_xml(root, out_path)
        written.append((os.path.relpath(out_path, args.out), ops))

    for relative, ops in written:
        print("  %-60s %d op(s)" % (relative, ops))
    for relative, reason in skipped:
        warn("skipped %s: %s" % (relative, reason))

    # A delta patches a file that already exists, so neither of these can be
    # expressed as one -- they are reported for the author to act on.
    for relative in only_modified:
        print("  NEW      %-59s not in the base game; ship the file itself" % relative)
    for relative in only_base:
        print("  MISSING  %-59s in the base game but absent here" % relative)

    print("%d delta(s) written to '%s', %d unchanged, %d new, %d missing, %d skipped"
          % (len(written), args.out, unchanged,
             len(only_modified), len(only_base), len(skipped)))
    return 0


def command_apply(args):
    if bool(args.out) == bool(args.in_place):
        print("ERROR: pass exactly one of --out or --in-place", file=sys.stderr)
        return 1

    applied, failed, backed_up = 0, 0, 0

    for dirpath, _, filenames in os.walk(args.delta_dir):
        for filename in sorted(filenames):
            full = os.path.join(dirpath, filename)
            delta_relative = os.path.relpath(full, args.delta_dir)

            source_relative = source_path_for(delta_relative)
            if source_relative is None:
                continue

            target_path = os.path.join(args.base_dir, source_relative)

            # Patch the unpatched file: the stash from an earlier --backup run if
            # there is one, otherwise the file itself. With --backup that makes
            # re-running idempotent; without it, re-running --in-place patches an
            # already patched map (noisy but not corrupting -- Remove reports a
            # missing target and Add is refused as a duplicate name).
            base_path = pristine_path_for(target_path)
            if not os.path.isfile(base_path):
                warn("skipped %s: '%s' does not exist in the base folder"
                     % (delta_relative, source_relative))
                failed += 1
                continue

            if args.in_place:
                if args.backup:
                    backup = backup_path_for(target_path)
                    if not os.path.isfile(backup):
                        shutil.copy2(target_path, backup)
                        backed_up += 1
                out_path = target_path
            else:
                out_path = os.path.join(args.out, source_relative)

            try:
                stats = apply_files(base_path, [full], out_path)
            except DeltaError as e:
                warn("skipped %s: %s" % (delta_relative, e))
                failed += 1
                continue

            print("  %-60s %s" % (source_relative, stats))
            if stats.skipped:
                failed += 1
            applied += 1

    where = "in place in '%s'" % args.base_dir if args.in_place else "to '%s'" % args.out
    print("%d file(s) written %s%s%s"
          % (applied, where,
             ", %d original(s) stashed" % backed_up if backed_up else "",
             ", %d with problems" % failed if failed else ""))

    return 1 if failed else 0



def command_show(args):
    paths = []
    if os.path.isdir(args.delta):
        for dirpath, _, filenames in os.walk(args.delta):
            for filename in sorted(filenames):
                if source_path_for(filename):
                    paths.append(os.path.join(dirpath, filename))
    else:
        paths.append(args.delta)

    for path in paths:
        root = parse_xml(path).getroot()
        print("%s  [%s Version=%s Target='%s' Name='%s' Priority=%s]"
              % (path, root.tag, root.get("Version", "1"), root.get("Target", ""),
                 root.get("Name", ""), root.get("Priority", "0")))

        counts = {}
        for op in root:
            if op.tag == "Add":
                for new in op:
                    print("    Add     %-14s %s" % (op.get("Category", "?"), new.get("Name", "?")))
                    counts["Add"] = counts.get("Add", 0) + 1
                continue

            if op.tag in ("Remove", "Modify"):
                print("    %-7s %-14s ID %-8s %s"
                      % (op.tag, op.get("Category", "?"), op.get("ID", "?"), op.get("Name", "")))
            else:
                print("    %s" % op.tag)
            counts[op.tag] = counts.get(op.tag, 0) + 1

        print("    --- " + ", ".join("%d x %s" % (n, t) for t, n in sorted(counts.items())))

    return 0


# --------------------------------------------------------------------------

def main(argv):
    parser = argparse.ArgumentParser(
        prog="mapdelta.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("diff", help="compare two game folders and write the deltas between them")
    p.add_argument("base_dir", help="pristine game folder")
    p.add_argument("modified_dir", help="folder holding your edited .map/.ent files")
    p.add_argument("--out", required=True, help="directory to write the delta tree into")
    p.add_argument("--name", default=None, help="mod name recorded in each delta")
    p.add_argument("--priority", type=int, default=0,
                   help="apply order when several mods patch one file (low first)")
    p.add_argument("--no-witness", action="store_true",
                   help="omit the Name attributes copied from the base file onto "
                        "Remove/Modify (they exist to detect a changed base)")
    p.add_argument("--prune", action="store_true",
                   help="delete delta files that no longer describe a change")
    p.set_defaults(func=command_diff)

    p = sub.add_parser("apply", help="patch the files a delta tree covers")
    p.add_argument("base_dir", help="game folder holding the files to patch")
    p.add_argument("delta_dir", help="directory holding the delta tree")
    p.add_argument("--out", default=None,
                   help="write the patched files into this directory instead, "
                        "leaving base_dir untouched (only files with a delta are "
                        "written; feed this straight back to 'diff')")
    p.add_argument("--in-place", action="store_true",
                   help="overwrite the maps in base_dir with the patched versions. "
                        "Intended for a working copy of the game; diff against your "
                        "pristine copy afterwards to regenerate the deltas.")
    p.add_argument("--backup", action="store_true",
                   help="with --in-place, stash each pristine original as <file>%s "
                        "first. Later runs read the stash as the base, which makes "
                        "re-running --in-place idempotent and lets "
                        "'diff <dir> <dir>' work against a single folder."
                        % BACKUP_SUFFIX)
    p.set_defaults(func=command_apply)

    p = sub.add_parser("show", help="summarise a delta file or a whole delta tree")
    p.add_argument("delta")
    p.set_defaults(func=command_show)

    args = parser.parse_args(argv)

    try:
        return args.func(args)
    except DeltaError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
