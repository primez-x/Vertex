"""Trusted, bounded library fallbacks for the isolated embedded import worker.

These functions never open files or resolve external references. The worker owns
input admission, process resource/time limits, source retention, and validation
of the returned candidate. Foreign IFC sections are measurement boundaries;
they make no claim about editable or manufacturing semantics.
"""
import io
import json
import math
from collections import Counter

MAX_INPUT_BYTES = 64 * 1024 * 1024
MAX_OUTPUT_BYTES = 16 * 1024 * 1024
MAX_DXF_ENTITIES = 100_000
MAX_INSERT_DEPTH = 16
MAX_IFC_PRODUCTS = 10_000
MAX_IFC_TRIANGLES = 1_000_000
MAX_IFC_VERTICES = 1_000_000
MAX_SECTION_POINTS = 100_000
MAX_SECTION_RINGS = 4096
NATIVE_DXF_APPID = "VERTEX_ENTITY_V1"
NATIVE_IFC_PSET = "Pset_VertexExchange_v1"


def _input(data):
    if not isinstance(data, bytes) or not data or len(data) > MAX_INPUT_BYTES:
        raise ValueError("invalid_or_excessive_cad_input")


def _diagnostic(entity_id, kind, code):
    return {"source_id": str(entity_id), "source_kind": str(kind), "code": code}


def _output(result):
    # Also bounds diagnostics and JSON escaping overhead at the IPC boundary.
    if len(json.dumps(result, ensure_ascii=False, allow_nan=False,
                      separators=(",", ":")).encode("utf-8")) > MAX_OUTPUT_BYTES:
        raise ValueError("cad_output_limit")
    return result


class _DxfWriter:
    def __init__(self):
        self.parts = []
        self.size = 0

    def put(self, code, value):
        if isinstance(value, float):
            if not math.isfinite(value) or abs(value) > 1e12:
                raise ValueError("invalid_dxf_coordinate")
            text = repr(value)
        elif isinstance(value, bytes):
            text = value.hex().upper()
        else:
            text = str(value)
        if any(ord(c) < 32 or ord(c) == 127 for c in text):
            raise ValueError("invalid_dxf_string")
        # DXF text format is UTF-8 from R2007. Native JSON XDATA must retain
        # its actual Unicode; ordinary strings use legal DXF Unicode escapes.
        if code != 1000:
            text = "".join(c if ord(c) < 128 else
                           "\\U+%04X" % ord(c) if ord(c) <= 0xffff else
                           "".join("\\U+%04X" % int.from_bytes(chunk, "big")
                                   for chunk in (c.encode("utf-16-be")[:2], c.encode("utf-16-be")[2:]))
                           for c in text)
        pair = str(code) + "\n" + text + "\n"
        self.size += len(pair.encode("utf-8"))
        if self.size > MAX_OUTPUT_BYTES:
            raise ValueError("cad_output_limit")
        self.parts.append(pair)

    def entity(self, entity, block_header=False):
        from ezdxf.lldxf.tagwriter import TagCollector
        collector = TagCollector(dxfversion="AC1027", write_handles=False, optional=False)
        entity.export_dxf(collector)
        # Do not turn an unstyled source dimension into a styled one through
        # ezdxf's generated defaults. Explicit source fields remain present
        # for the strict mapper to assess, including unsupported styles.
        absent_dimension_defaults = set()
        if entity.dxftype() == "DIMENSION":
            absent_dimension_defaults = {
                code for code, attribute in ((3, "dimstyle"),
                                             (71, "attachment_point"),
                                             (280, "version"))
                if not entity.dxf.hasattr(attribute)
            }
        for tag in collector.tags:
            if tag.code in absent_dimension_defaults:
                continue
            # Virtual/rebuilt entities have no handle/owner. Omit identifiers
            # consistently; they are not source identities in this candidate.
            if tag.code in (5, 330):
                continue
            # ezdxf always writes this optional, empty XREF path on BLOCK.
            # The native strict parser deliberately rejects external paths.
            if block_header and tag.code == 1 and not tag.value:
                continue
            self.put(tag.code, tag.value)

    def section(self, name):
        self.put(0, "SECTION")
        self.put(2, name)

    def end(self):
        self.put(0, "ENDSEC")


def _source_id(entity):
    original = entity.origin_of_copy or entity
    return original.dxf.get("handle", "") or ""


def normalize_dxf(data: bytes) -> dict:
    """Return minimal R2013 text, explicit fidelity diagnostics, retention flag.

    Foreign INSERTs are flattened only after a bounded graph preflight. Native
    metadata blocks/inserts retain the metadata and geometry for the C++ path.
    Unmapped entities remain in the retained source and have diagnostics.
    """
    _input(data)
    import ezdxf
    from ezdxf.document import Drawing
    from ezdxf.filemanagement import dxf_stream_info
    from ezdxf.lldxf.tagger import binary_tags_loader
    from ezdxf.xclip import XClip

    if data.startswith(b"AutoCAD Binary DXF\r\n\x1a\x00"):
        doc = Drawing.load(binary_tags_loader(data, errors="strict"))
    else:
        # Header syntax is ASCII even when strings use a legacy codepage.
        info = dxf_stream_info(io.StringIO(data.decode("latin1")))
        doc = ezdxf.read(io.StringIO(data.decode(info.encoding, errors="strict")))
    diagnostics = []
    native = {block.name for block in doc.blocks
              if block.block.has_xdata(NATIVE_DXF_APPID)}
    entity_count = 0
    # Preflight every source block, even if unused. No external/clipped sources
    # may slip through an unused definition or a library transform recursion.
    for block in doc.blocks:
        if block.block.dxf.flags & (4 | 8 | 16 | 32 | 64) or block.block.dxf.xref_path:
            raise ValueError("dxf_external_reference")
        for entity in block:
            entity_count += 1
            if entity_count > MAX_DXF_ENTITIES:
                raise ValueError("dxf_entity_limit")
            if entity.dxftype() == "INSERT" and XClip(entity).has_clipping_path:
                raise ValueError("dxf_clipping_unsupported")
    for entity in doc.modelspace():
        if entity.dxftype() == "INSERT" and XClip(entity).has_clipping_path:
            raise ValueError("dxf_clipping_unsupported")
    for layout in doc.layouts:
        if layout.name != "Model":
            diagnostics.extend(_diagnostic(_source_id(entity),entity.dxftype(),"dxf_paperspace_not_mapped") for entity in layout)

    def grid_count(insert):
        rows = int(insert.dxf.row_count)
        columns = int(insert.dxf.column_count)
        # multi_insert iterates the complete declared grid even at zero spacing.
        if rows < 1 or columns < 1 or rows * columns > MAX_DXF_ENTITIES:
            raise ValueError("dxf_insert_grid_limit")
        return rows * columns

    memo = {}

    def block_cost(name, path):
        if name in path:
            raise ValueError("dxf_insert_cycle")
        if len(path) >= MAX_INSERT_DEPTH:
            raise ValueError("dxf_insert_depth_limit")
        if name in memo:
            cost, height = memo[name]
            if len(path) + height > MAX_INSERT_DEPTH:
                raise ValueError("dxf_insert_depth_limit")
            return cost, height
        block = doc.blocks.get(name)
        if block is None:
            raise ValueError("dxf_missing_block")
        cost, height = 1, 1
        for entity in block:
            if entity.dxftype() == "INSERT":
                child_cost, child_height = block_cost(entity.dxf.name, path + (name,))
                cost += grid_count(entity) * child_cost
                height = max(height, child_height + 1)
                # The library recursively explodes sheared nested references,
                # which cannot preserve metadata-backed native INSERTs.
                scales = (abs(entity.dxf.xscale), abs(entity.dxf.yscale), abs(entity.dxf.zscale))
                child = doc.blocks.get(entity.dxf.name)
                if max(scales) - min(scales) > 1e-12 and any(e.dxftype() == "INSERT" for e in child):
                    raise ValueError("dxf_sheared_nested_insert")
            else:
                cost += 1
            if cost > MAX_DXF_ENTITIES:
                raise ValueError("dxf_entity_limit")
        memo[name] = cost, height
        return cost, height

    for block in doc.blocks:
        # Layout BLOCK records are containers, not an additional INSERT level.
        if not block.block_record.is_any_layout:
            block_cost(block.name, ())
    expansion = entity_count
    for entity in doc.modelspace():
        if entity.dxftype() == "INSERT":
            cost, _ = block_cost(entity.dxf.name, ())
            expansion += grid_count(entity) * cost
            child = doc.blocks.get(entity.dxf.name)
            scales = (abs(entity.dxf.xscale),abs(entity.dxf.yscale),abs(entity.dxf.zscale))
            if max(scales)-min(scales) > 1e-12 and any(e.dxftype()=="INSERT" for e in child):
                raise ValueError("dxf_sheared_nested_insert")
        else:
            expansion += 1
        if expansion > MAX_DXF_ENTITIES:
            raise ValueError("dxf_entity_limit")

    writer = _DxfWriter()
    writer.section("HEADER")
    writer.put(9, "$ACADVER")
    writer.put(1, "AC1027")
    writer.put(9, "$INSUNITS")
    writer.put(70, doc.header.get("$INSUNITS", 0))
    writer.end()
    if native:
        writer.section("TABLES")
        writer.put(0, "TABLE")
        writer.put(2, "APPID")
        writer.put(70, 1)
        writer.put(0, "APPID")
        writer.put(100, "AcDbSymbolTableRecord")
        writer.put(100, "AcDbRegAppTableRecord")
        writer.put(2, NATIVE_DXF_APPID)
        writer.put(70, 0)
        writer.put(0, "ENDTAB")
        writer.end()
        writer.section("BLOCKS")
        for block in doc.blocks:
            if block.name in native:
                writer.entity(block.block, block_header=True)
                # Export exact native geometry, including unsupported tags. The
                # strict C++ path decides activation; normalization cannot turn
                # unsupported native data into valid editable semantics.
                for entity in block:
                    writer.entity(entity)
                writer.entity(block.endblk)
        writer.end()
    writer.section("ENTITIES")
    written = 0
    mapped = {"LINE", "ARC", "LWPOLYLINE", "TEXT", "DIMENSION", "HATCH"}

    def emit(entity, depth=0, inherited_layer="0", fallback_source_id=""):
        nonlocal written
        written += 1
        if written > MAX_DXF_ENTITIES or depth > MAX_INSERT_DEPTH:
            raise ValueError("dxf_entity_limit")
        kind = entity.dxftype()
        source_id = _source_id(entity) or fallback_source_id
        if any(isinstance(value,str) and any(ord(c)>127 for c in value)
               for value in entity.dxf.all_existing_dxf_attribs().values()):
            diagnostics.append(_diagnostic(source_id,kind,"dxf_string_encoding_not_mapped"))
        layer = entity.dxf.get("layer", "0")
        if layer == "0" and inherited_layer != "0":
            entity = entity.copy()
            entity.dxf.layer = inherited_layer
            layer = inherited_layer
        if kind == "INSERT":
            if XClip(entity).has_clipping_path:
                raise ValueError("dxf_clipping_unsupported")
            if entity.attribs:
                diagnostics.append(_diagnostic(source_id, kind, "dxf_insert_attributes_not_mapped"))
            # Retain native INSERTs, expanding MINSERT into individual placements.
            instances = entity.multi_insert() if grid_count(entity) > 1 else (entity,)
            for instance in instances:
                if entity.dxf.name in native:
                    writer.entity(instance)
                    continue
                child_block = instance.block()
                for child in child_block:
                    if child.dxftype() == "ATTDEF":
                        diagnostics.append(_diagnostic(_source_id(child), "ATTDEF", "dxf_geometry_not_mapped"))
                def skipped(child, reason):
                    diagnostics.append(_diagnostic(_source_id(child), child.dxftype(), "dxf_insert_transform_not_mapped"))
                for child in instance.virtual_entities(skipped_entity_callback=skipped):
                    scales=(abs(instance.dxf.xscale),abs(instance.dxf.yscale),abs(instance.dxf.zscale))
                    if max(scales)-min(scales)>1e-12 and child.dxftype() in {"TEXT","MTEXT","ATTRIB","ATTDEF"}:
                        diagnostics.append(_diagnostic(_source_id(child) or source_id,child.dxftype(),"dxf_insert_transform_not_mapped"))
                        continue
                    emit(child, depth + 1, layer, source_id)
            return
        if kind == "POLYLINE":
            if not entity.is_2d_polyline or entity.dxf.flags & ~(1|128) or any(v.dxf.flags for v in entity.vertices):
                diagnostics.append(_diagnostic(source_id, kind, "dxf_geometry_not_mapped"))
                return
            # Exact old 2D polyline geometry, including bulges/widths/elevation.
            # Do not convert a mesh/polyface to a contour.
            poly = ezdxf.entities.LWPolyline.new(dxfattribs={
                "layer":layer, "flags":1 if entity.is_closed else 0,
                "elevation":entity.dxf.elevation.z,
                "extrusion":entity.dxf.extrusion,
                "thickness":entity.dxf.thickness})
            poly.set_points([(v.dxf.location.x,v.dxf.location.y,
                              v.dxf.get("start_width",entity.dxf.default_start_width),
                              v.dxf.get("end_width",entity.dxf.default_end_width),
                              v.dxf.bulge) for v in entity.vertices],format="xyseb")
            for name in ("color", "linetype", "lineweight", "true_color", "transparency"):
                if entity.dxf.hasattr(name):
                    poly.dxf.set(name, entity.dxf.get(name))
            if entity.has_xdata(NATIVE_DXF_APPID):
                poly.set_xdata(NATIVE_DXF_APPID, entity.get_xdata(NATIVE_DXF_APPID))
            writer.entity(poly)
        elif kind in mapped:
            writer.entity(entity)
        else:
            diagnostics.append(_diagnostic(source_id, kind, "dxf_geometry_not_mapped"))

    for entity in doc.modelspace():
        emit(entity)
    writer.end()
    writer.put(0, "EOF")
    return _output({"normalized_text":"".join(writer.parts),
                    "diagnostics":diagnostics,"source_retention_required":True})


def _signed_area(points):
    # Translate before multiplying: world offsets can exceed local dimensions
    # by twelve orders of magnitude, making the raw shoelace cancel to zero.
    x,y=points[0]
    return sum((a[0]-x)*(b[1]-y)-(b[0]-x)*(a[1]-y)
               for a,b in zip(points,points[1:]+points[:1]))/2


def _section_loops(vertices, faces):
    """Intersect the triangulated product with its horizontal mid-height plane.

    Polygonize linework, never the filled triangles or a convex hull. Unique
    rings are classified by containment parity so holes and components survive.
    Ambiguous/open linework is rejected rather than repaired into invented area.
    """
    from shapely.geometry import LineString, Polygon
    from shapely.ops import polygonize_full, unary_union

    low = min(p[2] for p in vertices)
    high = max(p[2] for p in vertices)
    level = (low + high) / 2
    # A relative tolerance for extent and ULPs for large world translations.
    span = max(max(p[i] for p in vertices)-min(p[i] for p in vertices) for i in range(3))
    scale = max(abs(c) for p in vertices for c in p)
    tolerance = max(1e-9,span*1e-9,math.ulp(scale)*8)
    segments = set()
    coplanar = Counter()
    point_values = {}

    def key(point):
        k = (round(point[0]/tolerance),round(point[1]/tolerance))
        point_values.setdefault(k,(point[0],point[1]))
        return k

    def edge(a,b):
        a,b=key(a),key(b)
        return tuple(sorted((a,b))) if a!=b else None

    for indices in faces:
        triangle = [vertices[i] for i in indices]
        distances = [p[2]-level for p in triangle]
        on = [abs(d)<=tolerance for d in distances]
        if all(on):
            for a,b in zip(triangle,triangle[1:]+triangle[:1]):
                e=edge(a,b)
                if e:
                    coplanar[e]+=1
            if len(segments)+len(coplanar)>MAX_SECTION_POINTS:
                raise ValueError("ifc_section_limit")
            continue
        points = [p for p,zero in zip(triangle,on) if zero]
        for i,j in ((0,1),(1,2),(2,0)):
            if not on[i] and not on[j] and (distances[i]<0)!=(distances[j]<0):
                t=distances[i]/(distances[i]-distances[j])
                a,b=triangle[i],triangle[j]
                points.append(tuple(a[n]+t*(b[n]-a[n]) for n in range(3)))
        unique = {key(p):p for p in points}
        if len(unique)==2:
            a,b=unique
            segments.add(tuple(sorted((a,b))))
        elif len(unique)>2:
            raise ValueError("ifc_section_ambiguous")
        if len(segments)+len(coplanar)>MAX_SECTION_POINTS:
            raise ValueError("ifc_section_limit")
    # Internal diagonals of planar triangulations occur twice and cancel.
    # More than two facets incident on an edge is not a manifold measurement.
    if any(count>2 for count in coplanar.values()):
        raise ValueError("ifc_section_ambiguous")
    segments.update(e for e,count in coplanar.items() if count==1)
    if not segments:
        return [],[]
    linework=unary_union([LineString([point_values[a],point_values[b]]) for a,b in sorted(segments)])
    polygons,cuts,dangles,invalid=polygonize_full(linework)
    if not cuts.is_empty or not dangles.is_empty or not invalid.is_empty:
        raise ValueError("ifc_section_not_closed")
    unique_rings={}
    for polygon in polygons.geoms:
        for ring in (polygon.exterior,*polygon.interiors):
            coordinates=list(ring.coords)[:-1]
            keys=[key(p) for p in coordinates]
            # Canonical, direction-independent ring identity. Containment, not
            # polygonizer output order, defines solid/void parity.
            smallest=min(range(len(keys)),key=keys.__getitem__)
            forward=tuple(keys[smallest:]+keys[:smallest])
            backward=(forward[0],*reversed(forward[1:]))
            unique_rings[min(forward,backward)]=coordinates
            if len(unique_rings)>MAX_SECTION_RINGS:
                raise ValueError("ifc_section_limit")
    rings=[(coords,Polygon(coords)) for coords in unique_rings.values()]
    if sum(len(coords) for coords,_ in rings)>MAX_SECTION_POINTS:
        raise ValueError("ifc_section_limit")
    classified=[]
    for coords,polygon in rings:
        if not polygon.is_valid or polygon.area<=tolerance*tolerance:
            raise ValueError("ifc_section_ambiguous")
        containers=0
        for other_coords,other in rings:
            if other is polygon:
                continue
            if other.boundary.intersects(polygon.boundary):
                raise ValueError("ifc_section_ambiguous")
            if other.contains(polygon):
                containers+=1
        signed=_signed_area(coords)
        outer=containers%2==0
        if (signed>0)!=outer:
            coords=list(reversed(coords))
        classified.append(([[float(x),float(y)] for x,y in coords],"outer" if outer else "hole",abs(signed)))
    classified.sort(key=lambda item:(min(p[0] for p in item[0]),min(p[1] for p in item[0]),-item[2]))
    return [item[0] for item in classified],[item[1] for item in classified]


def project_ifc(data: bytes) -> dict:
    """Return SI world-coordinate measurement sections for foreign products.

    IfcOpenShell parses/extracts geometry. Each product uses a disclosed default
    cut at its own bounding-box mid-height. No IFC class becomes a manufactured
    wall/slab/opening; native metadata products belong exclusively to C++.
    """
    _input(data)
    import ifcopenshell
    import ifcopenshell.geom

    text=data.decode("utf-8",errors="strict")
    if "\x00" in text:
        raise ValueError("invalid_ifc_input")
    model=ifcopenshell.file.from_string(text)
    projects=model.by_type("IfcProject")
    assignment=projects[0].UnitsInContext if len(projects)==1 else None
    units=[unit for unit in assignment.Units if getattr(unit,"UnitType",None)=="LENGTHUNIT"] if assignment else []
    resolved=len(units)==1
    unit=units[0] if resolved else None
    visited=set()
    while resolved and unit.is_a("IfcConversionBasedUnit"):
        if unit.id() in visited or len(visited)>=8:
            resolved=False
            break
        visited.add(unit.id())
        factor=unit.ConversionFactor.ValueComponent.wrappedValue
        if not isinstance(factor,(int,float)) or not math.isfinite(factor) or factor<=0 or getattr(unit,"ConversionOffset",0):
            resolved=False
            break
        unit=unit.ConversionFactor.UnitComponent
    resolved=resolved and unit.is_a("IfcSIUnit") and unit.Name=="METRE" and unit.Prefix in (
        None,"EXA","PETA","TERA","GIGA","MEGA","KILO","HECTO","DECA","DECI","CENTI","MILLI","MICRO","NANO","PICO","FEMTO","ATTO")
    if not resolved:
        return _output({"boundaries":[],"diagnostics":[_diagnostic(
            projects[0].id() if projects else "", "IfcProject", "ifc_length_units_unresolved")]})
    products=model.by_type("IfcProduct")
    if len(products)>MAX_IFC_PRODUCTS:
        raise ValueError("ifc_product_limit")
    native=set()
    for relation in model.by_type("IfcRelDefinesByProperties"):
        definitions=relation.RelatingPropertyDefinition
        definitions=definitions if isinstance(definitions,tuple) else (definitions,)
        if any(definition and definition.is_a("IfcPropertySet") and definition.Name==NATIVE_IFC_PSET for definition in definitions):
            native.update(product.id() for product in relation.RelatedObjects)
    settings=ifcopenshell.geom.settings()
    settings.set("use-world-coords",True)
    settings.set("convert-back-units",False)
    settings.set("mesher-linear-deflection",0.0001)
    settings.set("mesher-angular-deflection",0.01)
    boundaries=[]
    diagnostics=[]
    triangle_count=0
    vertex_count=0
    section_points=0
    for product in products:
        if product.id() in native:
            continue
        source_id=str(product.id())
        source_kind=product.is_a()
        if not product.Representation:
            if source_kind in ("IfcSite", "IfcBuilding", "IfcBuildingStorey"):
                # Organizational containers normally have no drawable body.
                continue
            diagnostics.append(_diagnostic(source_id,source_kind,"ifc_geometry_unavailable"))
            continue
        try:
            shape=ifcopenshell.geom.create_shape(settings,product)
        except (RuntimeError,ValueError):
            diagnostics.append(_diagnostic(source_id,source_kind,"ifc_geometry_unavailable"))
            continue
        coordinates=shape.geometry.verts
        indices=shape.geometry.faces
        if len(coordinates)%3 or len(indices)%3:
            raise ValueError("invalid_ifc_mesh")
        triangle_count+=len(indices)//3
        vertex_count+=len(coordinates)//3
        if triangle_count>MAX_IFC_TRIANGLES or vertex_count>MAX_IFC_VERTICES:
            raise ValueError("ifc_mesh_limit")
        if not coordinates or not indices:
            diagnostics.append(_diagnostic(source_id,source_kind,"ifc_geometry_unavailable"))
            continue
        if any(not math.isfinite(c) or abs(c)>1e12 for c in coordinates):
            raise ValueError("invalid_ifc_coordinate")
        vertices=[coordinates[i:i+3] for i in range(0,len(coordinates),3)]
        faces=[indices[i:i+3] for i in range(0,len(indices),3)]
        if any(i<0 or i>=len(vertices) for i in indices):
            raise ValueError("invalid_ifc_mesh")
        try:
            loops,roles=_section_loops(vertices,faces)
        except ValueError as error:
            if str(error)=="ifc_section_limit":
                raise
            diagnostics.append(_diagnostic(source_id,source_kind,str(error)))
            continue
        if not loops:
            diagnostics.append(_diagnostic(source_id,source_kind,"ifc_section_empty"))
            continue
        section_points+=sum(len(loop) for loop in loops)
        if section_points>MAX_SECTION_POINTS:
            raise ValueError("ifc_section_limit")
        boundaries.append({"source_id":source_id,"source_kind":source_kind,
                           "loops":loops,"roles":roles,"approximation":True})
        diagnostics.append(_diagnostic(source_id,source_kind,"external_ifc_midheight_section"))
    return _output({"boundaries":boundaries,"diagnostics":diagnostics})
