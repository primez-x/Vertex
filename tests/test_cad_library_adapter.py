"""Real-library adapter regressions; run with the locked isolated interpreter.

The test runner adds only the trusted adapter directory to the isolated path.
"""
import importlib.util
import io
import pathlib
import sys
import unittest
from unittest.mock import patch

import ezdxf
import ifcopenshell
import ifcopenshell.guid

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "cad_library_adapter", ROOT / "src/desktop/cad_library_adapter.py")
adapter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(adapter)


def dxf_bytes(doc, binary=False):
    stream = io.BytesIO() if binary else io.StringIO()
    doc.write(stream, fmt="bin" if binary else "asc")
    result = stream.getvalue()
    return result if binary else result.encode(doc.output_encoding)


def normalized(doc, binary=False):
    result = adapter.normalize_dxf(dxf_bytes(doc, binary))
    return result, ezdxf.read(io.StringIO(result["normalized_text"]))


def ifc_model():
    model = ifcopenshell.file(schema="IFC4")
    origin = model.create_entity("IfcCartesianPoint", (0., 0., 0.))
    axis = model.create_entity("IfcAxis2Placement3D", origin, None, None)
    context = model.create_entity("IfcGeometricRepresentationContext", None, "Model", 3, 1e-7, axis, None)
    units = model.create_entity("IfcUnitAssignment", [model.create_entity("IfcSIUnit", None, "LENGTHUNIT", None, "METRE")])
    model.create_entity("IfcProject", GlobalId=ifcopenshell.guid.new(), RepresentationContexts=[context], UnitsInContext=units)
    return model, context


def add_swept(model, context, hollow=False, transform=True):
    if hollow:
        def ring(points):
            return model.create_entity("IfcPolyline", [model.create_entity("IfcCartesianPoint", p) for p in points])
        outer = ring([(0., 0.), (4., 0.), (4., 4.), (0., 4.), (0., 0.)])
        inner = ring([(1., 1.), (1., 3.), (3., 3.), (3., 1.), (1., 1.)])
        profile = model.create_entity("IfcArbitraryProfileDefWithVoids", "AREA", None, outer, [inner])
    else:
        profile = model.create_entity("IfcRectangleProfileDef", "AREA", None, None, 4., 2.)
    zero = model.create_entity("IfcCartesianPoint", (0., 0., 0.))
    axis = model.create_entity("IfcAxis2Placement3D", zero, None, None)
    direction = model.create_entity("IfcDirection", (0., 0., 1.))
    solid = model.create_entity("IfcExtrudedAreaSolid", profile, axis, direction, 3.)
    rep = model.create_entity("IfcShapeRepresentation", context, "Body", "SweptSolid", [solid])
    definition = model.create_entity("IfcProductDefinitionShape", None, None, [rep])
    location = model.create_entity("IfcCartesianPoint", (10., 20., 5.) if transform else (0., 0., 0.))
    ref = model.create_entity("IfcDirection", (0., 1., 0.)) if transform else None
    placement = model.create_entity("IfcLocalPlacement", None, model.create_entity("IfcAxis2Placement3D", location, None, ref))
    return model.create_entity("IfcBuildingElementProxy", GlobalId=ifcopenshell.guid.new(), ObjectPlacement=placement, Representation=definition)


def add_mesh(model, context, flat=False):
    vertices = []
    triangles = []
    for x in (0., 3.):
        offset = len(vertices)
        vertices += [(x, 0., 0.), (x+1., 0., 0.), (x+1., 1., 0.), (x, 1., 0.),
                     (x, 0., 1.), (x+1., 0., 1.), (x+1., 1., 1.), (x, 1., 1.)]
        faces = [(1,3,2), (1,4,3)] if flat else [(1,3,2),(1,4,3),(5,6,7),(5,7,8),(1,2,6),(1,6,5),(2,3,7),(2,7,6),(3,4,8),(3,8,7),(4,1,5),(4,5,8)]
        triangles += [tuple(i+offset for i in t) for t in faces]
    if flat:
        vertices = [(x,y,0.) for x,y,z in vertices]
    points = model.create_entity("IfcCartesianPointList3D", vertices)
    body = model.create_entity("IfcTriangulatedFaceSet", points, None, not flat, triangles, None)
    rep = model.create_entity("IfcShapeRepresentation", context, "Body", "Tessellation", [body])
    return model.create_entity("IfcBuildingElementProxy", GlobalId=ifcopenshell.guid.new(), Representation=model.create_entity("IfcProductDefinitionShape", None, None, [rep]))


def area(loop):
    return sum(a[0]*b[1]-b[0]*a[1] for a,b in zip(loop,loop[1:]+loop[:1]))/2


class DxfAdapterTests(unittest.TestCase):
    @staticmethod
    def plain_dimension_bytes(common_tags=""):
        # Minimal rotated dimension in the same bounded form as Vertex export.
        return ("0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1027\n"
                "9\n$INSUNITS\n70\n6\n0\nENDSEC\n0\nSECTION\n2\nENTITIES\n"
                "0\nDIMENSION\n100\nAcDbEntity\n8\nDimensions\n100\nAcDbDimension\n"
                "10\n2\n20\n1\n30\n0\n11\n2\n21\n1.5\n31\n0\n70\n0\n"
                "1\nFour metres\n" + common_tags +
                "100\nAcDbAlignedDimension\n13\n0\n23\n0\n33\n0\n"
                "14\n4\n24\n0\n34\n0\n50\n30\n100\nAcDbRotatedDimension\n"
                "0\nENDSEC\n0\nEOF\n").encode()

    def test_plain_dimension_keeps_geometry_without_generated_style(self):
        result = adapter.normalize_dxf(self.plain_dimension_bytes())
        text = result["normalized_text"]
        parsed = ezdxf.read(io.StringIO(text)).modelspace()[0]
        self.assertEqual(tuple(parsed.dxf.defpoint2), (0, 0, 0))
        self.assertEqual(tuple(parsed.dxf.defpoint3), (4, 0, 0))
        self.assertEqual(parsed.dxf.angle, 30)
        self.assertEqual(parsed.dxf.layer, "Dimensions")
        self.assertEqual(parsed.dxf.text, "Four metres")
        for code in (3, 71, 280):
            self.assertNotIn(f"\n{code}\n", text)

    def test_explicit_dimension_style_metadata_is_never_erased(self):
        for source_tags, expected_tag in (("3\nCustomStyle\n", "3\nCustomStyle\n"),
                                          ("71\n5\n", "71\n5\n"),
                                          ("280\n0\n", "280\n0\n")):
            with self.subTest(source_tags=source_tags):
                result = adapter.normalize_dxf(self.plain_dimension_bytes(source_tags))
                self.assertIn(expected_tag, result["normalized_text"])

    def test_binary_line_is_r2013_ascii_with_coordinates(self):
        doc = ezdxf.new("R2013")
        doc.modelspace().add_line((1,2), (4,6))
        result, parsed = normalized(doc, True)
        self.assertEqual(parsed.dxfversion, "AC1027")
        line = parsed.modelspace()[0]
        self.assertEqual(tuple(line.dxf.start), (1,2,0))
        self.assertEqual(tuple(line.dxf.end), (4,6,0))
        self.assertTrue(result["source_retention_required"])

    def test_old_polyline_and_declared_codepage_are_normalized(self):
        doc = ezdxf.document.Drawing.new("R12")
        doc.header["$INSUNITS"] = 0
        doc.encoding = "cp1251"
        doc.modelspace().add_polyline2d([(0,0), (2,1)], close=True)
        doc.modelspace().add_text("Привет")
        result, parsed = normalized(doc)
        poly = parsed.modelspace()[0]
        self.assertEqual(poly.dxftype(), "LWPOLYLINE")
        self.assertTrue(poly.closed)
        self.assertEqual(parsed.modelspace()[1].dxf.text, "\\U+041F\\U+0440\\U+0438\\U+0432\\U+0435\\U+0442")
        self.assertTrue(any(d["code"]=="dxf_string_encoding_not_mapped" for d in result["diagnostics"]))

    def test_nested_rotated_multi_insert_uses_world_coordinates(self):
        doc = ezdxf.new("R2013")
        inner = doc.blocks.new("INNER")
        inner.add_line((0,0), (1,0))
        outer = doc.blocks.new("OUTER")
        outer.add_blockref("INNER", (2,0))
        doc.modelspace().add_blockref("OUTER", (10,20), dxfattribs={"rotation":90,"column_count":2,"column_spacing":3})
        result, parsed = normalized(doc)
        lines = list(parsed.modelspace())
        self.assertEqual(len(lines), 2)
        self.assertEqual([line.dxftype() for line in lines], ["LINE","LINE"])
        for line,start,end in zip(lines, [(10,22),(10,25)], [(10,23),(10,26)]):
            self.assertAlmostEqual(line.dxf.start.x,start[0])
            self.assertAlmostEqual(line.dxf.start.y,start[1])
            self.assertAlmostEqual(line.dxf.end.x,end[0])
            self.assertAlmostEqual(line.dxf.end.y,end[1])

    def test_native_metadata_block_and_insert_are_preserved(self):
        doc = ezdxf.new("R2013")
        doc.appids.new("VERTEX_ENTITY_V1")
        block = doc.blocks.new("NATIVE")
        block.block.set_xdata("VERTEX_ENTITY_V1", [(1000,'{"native":true}')])
        block.add_line((0,0),(1,0))
        doc.modelspace().add_blockref("NATIVE", (10,20))
        result, parsed = normalized(doc)
        self.assertEqual(parsed.modelspace()[0].dxftype(), "INSERT")
        self.assertEqual(parsed.blocks["NATIVE"].block.get_xdata("VERTEX_ENTITY_V1")[0].value, '{"native":true}')

    def test_cycles_and_clipping_and_xref_are_rejected(self):
        for mode in ("cycle", "clip", "xref"):
            with self.subTest(mode=mode):
                doc = ezdxf.new("R2013")
                block = doc.blocks.new("BLOCK")
                block.add_line((0,0),(1,0))
                placed = doc.modelspace().add_blockref("BLOCK", (0,0))
                if mode == "cycle":
                    block.add_blockref("BLOCK", (0,0))
                elif mode == "clip":
                    from ezdxf.xclip import XClip
                    XClip(placed).set_block_clipping_path([(0,0),(1,0),(1,1),(0,1)])
                else:
                    block.block.dxf.flags = 4
                with self.assertRaises(ValueError):
                    adapter.normalize_dxf(dxf_bytes(doc))

    def test_entity_depth_input_and_output_budgets_reject(self):
        doc = ezdxf.new("R2013")
        block = doc.blocks.new("BLOCK")
        block.add_line((0,0),(1,0))
        doc.modelspace().add_blockref("BLOCK", (0,0), dxfattribs={"column_count":20,"column_spacing":1})
        for limit,value in (("MAX_DXF_ENTITIES", 10),("MAX_OUTPUT_BYTES",100),("MAX_INPUT_BYTES",100)):
            with self.subTest(limit=limit), patch.object(adapter, limit, value), self.assertRaises(ValueError):
                adapter.normalize_dxf(dxf_bytes(doc))
        for i in range(18):
            parent = doc.blocks.new(f"DEPTH{i}")
            parent.add_blockref("BLOCK" if i==0 else f"DEPTH{i-1}",(0,0))
        doc.modelspace().add_blockref("DEPTH17",(0,0))
        with self.assertRaises(ValueError):
            adapter.normalize_dxf(dxf_bytes(doc))

    def test_unsupported_geometry_has_source_diagnostic(self):
        doc = ezdxf.new("R2013")
        ellipse = doc.modelspace().add_ellipse((0,0), major_axis=(2,0), ratio=0.5)
        result,_ = normalized(doc)
        self.assertIn({"source_id":ellipse.dxf.handle,"source_kind":"ELLIPSE","code":"dxf_geometry_not_mapped"}, result["diagnostics"])

    def test_circle_ascii_and_binary_preserve_exact_geometry(self):
        for binary in (False, True):
            with self.subTest(binary=binary):
                doc = ezdxf.new("R2013")
                doc.header["$INSUNITS"] = 4
                doc.modelspace().add_circle((3000,-1000),2000,dxfattribs={"layer":"Round pads"})
                result, parsed = normalized(doc, binary)
                circle = parsed.modelspace()[0]
                self.assertEqual(circle.dxftype(), "CIRCLE")
                self.assertEqual(tuple(circle.dxf.center), (3000,-1000,0))
                self.assertEqual(circle.dxf.radius, 2000)
                self.assertEqual(circle.dxf.layer, "Round pads")
                self.assertEqual(parsed.header["$INSUNITS"], 4)
                self.assertEqual(result["diagnostics"], [])

    def test_circle_uniform_and_reflected_insert_preserve_exact_geometry(self):
        for xscale in (2, -2):
            with self.subTest(xscale=xscale):
                doc = ezdxf.new("R2013")
                block = doc.blocks.new("ROUND")
                block.add_circle((1,3),2,dxfattribs={"color":3,"thickness":0.5})
                doc.modelspace().add_blockref("ROUND",(10,20),dxfattribs={
                    "xscale":xscale,"yscale":2,"zscale":2,"layer":"Round pads"})
                result, parsed = normalized(doc)
                circle = parsed.modelspace()[0]
                self.assertEqual(circle.dxftype(), "CIRCLE")
                self.assertEqual(circle.dxf.radius, 4)
                self.assertEqual(tuple(circle.dxf.extrusion), (0,0,1))
                center = circle.ocs().to_wcs(circle.dxf.center)
                self.assertEqual(tuple(center), (10+xscale,26,0))
                self.assertEqual(circle.dxf.layer, "Round pads")
                self.assertEqual(circle.dxf.color, 3)
                self.assertEqual(abs(circle.dxf.thickness), 1)
                self.assertEqual(result["diagnostics"], [])

    def test_raw_and_block_nondefault_circle_ocs_is_preserved(self):
        for in_block in (False, True):
            with self.subTest(in_block=in_block):
                doc = ezdxf.new("R2013")
                target = doc.blocks.new("ROUND") if in_block else doc.modelspace()
                target.add_circle((1,2),3,dxfattribs={"extrusion":(0,0,-1),"color":3})
                if in_block:
                    doc.modelspace().add_blockref("ROUND",(0,0))
                _, parsed = normalized(doc)
                if in_block:
                    self.assertEqual(len(parsed.modelspace()), 0)
                    continue
                circle = parsed.modelspace()[0]
                self.assertEqual(tuple(circle.dxf.extrusion), (0,0,-1))
                self.assertEqual(circle.dxf.color, 3)

    def test_circle_insert_cannot_rehabilitate_invalid_source_geometry(self):
        cases = (
            ((0,0,0), 2, (0,0,-1), {"xscale":-1}),
            ((0,0,0), 2, (0,1,0), {"extrusion":(0,1,0)}),
            ((0,0,0), -2, (0,0,1), {}),
            ((0,0,2), 2, (0,0,1), {}),
        )
        for center, radius, extrusion, attrs in cases:
            with self.subTest(center=center, radius=radius, extrusion=extrusion, attrs=attrs):
                doc = ezdxf.new("R2013")
                block = doc.blocks.new("ROUND")
                original = block.add_circle(center,radius,dxfattribs={"extrusion":extrusion})
                placement = (0,0,-2) if center[2] else (0,0,0)
                doc.modelspace().add_blockref("ROUND",placement,dxfattribs=attrs)
                result, parsed = normalized(doc)
                self.assertEqual(len(parsed.modelspace()), 0)
                self.assertIn({"source_id":original.dxf.handle,"source_kind":"CIRCLE",
                               "code":"dxf_insert_transform_not_mapped"}, result["diagnostics"])

    def test_circle_insert_preserves_valid_upscaled_tiny_source_radius(self):
        doc = ezdxf.new("R2013")
        block = doc.blocks.new("ROUND")
        block.add_circle((0,0),1e-9)
        doc.modelspace().add_blockref("ROUND",(0,0),dxfattribs={"xscale":1e9,"yscale":1e9})
        result, parsed = normalized(doc)
        self.assertEqual(parsed.modelspace()[0].dxf.radius, 1)
        self.assertEqual(result["diagnostics"], [])

    def test_nonuniform_circle_insert_reports_unmapped_ellipse(self):
        doc = ezdxf.new("R2013")
        block = doc.blocks.new("ROUND")
        block.add_circle((0,0),2)
        doc.modelspace().add_blockref("ROUND",(0,0),dxfattribs={"xscale":2})
        result, parsed = normalized(doc)
        self.assertEqual(len(parsed.modelspace()), 0)
        self.assertTrue(any(d["source_kind"] == "ELLIPSE" and
                            d["code"] == "dxf_geometry_not_mapped" for d in result["diagnostics"]))

    def test_library_tolerance_cannot_rehabilitate_nonuniform_circle_insert(self):
        for radius, sx, sy in ((100000, 1e-5, 2e-5), (2, 1, 1+1e-12)):
            with self.subTest(radius=radius, sx=sx, sy=sy):
                doc = ezdxf.new("R2013")
                block = doc.blocks.new("ROUND")
                block.add_circle((0,0),radius)
                doc.modelspace().add_blockref("ROUND",(0,0),dxfattribs={"xscale":sx,"yscale":sy})
                result, parsed = normalized(doc)
                self.assertEqual(len(parsed.modelspace()), 0)
                self.assertTrue(any(d["source_kind"] == "CIRCLE" and
                                    d["code"] == "dxf_insert_transform_not_mapped"
                                    for d in result["diagnostics"]))
                self.assertTrue(result["source_retention_required"])

    def test_tilted_circle_preserves_extrusion_for_strict_mapper(self):
        doc = ezdxf.new("R2013")
        doc.modelspace().add_circle((1,2),3,dxfattribs={"extrusion":(0,1,0)})
        _, parsed = normalized(doc)
        circle = parsed.modelspace()[0]
        self.assertEqual(circle.dxftype(), "CIRCLE")
        self.assertEqual(tuple(circle.dxf.extrusion), (0,1,0))

    def test_layer_zero_inherits_foreign_insert_layer(self):
        doc = ezdxf.new("R2013")
        block = doc.blocks.new("PART")
        block.add_line((0,0),(1,0))
        doc.modelspace().add_blockref("PART",(0,0),dxfattribs={"layer":"Walls"})
        result,parsed = normalized(doc)
        self.assertEqual(parsed.modelspace()[0].dxf.layer,"Walls")

    def test_unsupported_native_geometry_is_preserved_for_strict_validation(self):
        doc=ezdxf.new("R2013")
        doc.appids.new("VERTEX_ENTITY_V1")
        block=doc.blocks.new("NATIVE")
        block.block.set_xdata("VERTEX_ENTITY_V1",[(1000,'{"native":true}')])
        block.add_circle((0,0),2)
        doc.modelspace().add_blockref("NATIVE",(0,0))
        result,parsed=normalized(doc)
        self.assertEqual(parsed.blocks["NATIVE"][0].dxftype(),"CIRCLE")
        self.assertEqual(parsed.modelspace()[0].dxftype(),"INSERT")

    def test_sheared_nested_insert_is_rejected_before_library_recursion(self):
        doc=ezdxf.new("R2013")
        inner=doc.blocks.new("INNER")
        inner.add_line((0,0),(1,0))
        outer=doc.blocks.new("OUTER")
        outer.add_blockref("INNER",(0,0),dxfattribs={"rotation":45})
        doc.modelspace().add_blockref("OUTER",(0,0),dxfattribs={"xscale":2})
        with self.assertRaisesRegex(ValueError,"sheared_nested"):
            adapter.normalize_dxf(dxf_bytes(doc))

    def test_sixteen_insert_levels_are_accepted(self):
        doc=ezdxf.new("R2013")
        doc.blocks.new("LEVEL0").add_line((0,0),(1,0))
        for i in range(1,16):
            doc.blocks.new(f"LEVEL{i}").add_blockref(f"LEVEL{i-1}",(1,0))
        doc.modelspace().add_blockref("LEVEL15",(1,0))
        result,parsed=normalized(doc)
        self.assertEqual(tuple(parsed.modelspace()[0].dxf.start),(16,0,0))

    def test_nonuniform_insert_text_has_fidelity_diagnostic(self):
        doc=ezdxf.new("R2013")
        block=doc.blocks.new("LABEL")
        block.add_text("label")
        doc.modelspace().add_blockref("LABEL",(0,0),dxfattribs={"xscale":2})
        result,parsed=normalized(doc)
        self.assertEqual(len(parsed.modelspace()),0)
        self.assertTrue(any(d["code"]=="dxf_insert_transform_not_mapped" for d in result["diagnostics"]))

    def test_paperspace_geometry_is_explicitly_diagnosed(self):
        doc=ezdxf.new("R2013")
        line=doc.layout("Layout1").add_line((0,0),(1,0))
        result,parsed=normalized(doc)
        self.assertIn({"source_id":line.dxf.handle,"source_kind":"LINE","code":"dxf_paperspace_not_mapped"},result["diagnostics"])


class IfcAdapterTests(unittest.TestCase):
    def test_swept_rotated_translated_product_section_is_world_si(self):
        model, context = ifc_model()
        product = add_swept(model,context)
        result = adapter.project_ifc(model.to_string().encode())
        boundary = result["boundaries"][0]
        self.assertEqual(boundary["source_id"], str(product.id()))
        self.assertTrue(boundary["approximation"])
        loop = boundary["loops"][0]
        self.assertAlmostEqual(abs(area(loop)),8.)
        self.assertAlmostEqual(min(p[0] for p in loop),9.)
        self.assertAlmostEqual(max(p[0] for p in loop),11.)
        self.assertAlmostEqual(min(p[1] for p in loop),18.)
        self.assertAlmostEqual(max(p[1] for p in loop),22.)
        self.assertTrue(any(d["code"]=="external_ifc_midheight_section" for d in result["diagnostics"]))

    def test_hollow_profile_preserves_outer_and_hole_orientation(self):
        model, context = ifc_model()
        add_swept(model,context, hollow=True, transform=False)
        loops = adapter.project_ifc(model.to_string().encode())["boundaries"][0]["loops"]
        self.assertEqual(len(loops),2)
        self.assertEqual(sorted(round(area(loop),6) for loop in loops),[-4.,16.])
        boundary=adapter.project_ifc(model.to_string().encode())["boundaries"][0]
        self.assertEqual(boundary["roles"],["outer","hole"])

    def test_millimetre_source_uses_si_world_coordinates(self):
        model,context=ifc_model()
        model.by_type("IfcSIUnit")[0].Prefix="MILLI"
        add_swept(model,context)
        loop=adapter.project_ifc(model.to_string().encode())["boundaries"][0]["loops"][0]
        self.assertAlmostEqual(min(p[0] for p in loop),.009)
        self.assertAlmostEqual(max(p[1] for p in loop),.022)
        self.assertAlmostEqual(abs(area(loop)),.000008)

    def test_disconnected_mesh_preserves_both_components(self):
        model, context = ifc_model()
        add_mesh(model,context)
        loops = adapter.project_ifc(model.to_string().encode())["boundaries"][0]["loops"]
        self.assertEqual(len(loops),2)
        self.assertEqual(sorted(round(area(loop),6) for loop in loops),[1.,1.])

    def test_flat_coplanar_facets_cancel_internal_triangle_edges(self):
        model, context = ifc_model()
        add_mesh(model,context,flat=True)
        loops = adapter.project_ifc(model.to_string().encode())["boundaries"][0]["loops"]
        self.assertEqual(len(loops),2)
        self.assertEqual(sorted(round(area(loop),6) for loop in loops),[1.,1.])

    def test_native_metadata_product_is_left_for_native_path(self):
        for envelope in ("Pset_VertexExchange_v1", "Pset_VertexExchange_v2"):
            with self.subTest(envelope=envelope):
                model, context = ifc_model()
                product = add_swept(model,context)
                pset = model.create_entity("IfcPropertySet", GlobalId=ifcopenshell.guid.new(), Name=envelope, HasProperties=[])
                model.create_entity("IfcRelDefinesByProperties", GlobalId=ifcopenshell.guid.new(), RelatedObjects=[product], RelatingPropertyDefinition=pset)
                with patch("ifcopenshell.geom.create_shape", side_effect=AssertionError("Native metadata must not be re-sectioned")):
                    self.assertEqual(adapter.project_ifc(model.to_string().encode())["boundaries"],[])

    def test_unknown_native_metadata_is_not_silently_excluded(self):
        model, context = ifc_model()
        product = add_swept(model,context)
        pset = model.create_entity("IfcPropertySet", GlobalId=ifcopenshell.guid.new(), Name="Pset_VertexExchange_v3", HasProperties=[])
        model.create_entity("IfcRelDefinesByProperties", GlobalId=ifcopenshell.guid.new(), RelatedObjects=[product], RelatingPropertyDefinition=pset)
        self.assertEqual(len(adapter.project_ifc(model.to_string().encode())["boundaries"]),1)

    def test_large_world_translation_keeps_hole_roles(self):
        model,context=ifc_model()
        product=add_swept(model,context,hollow=True)
        product.ObjectPlacement.RelativePlacement.Location.Coordinates=(1e9,2e9,5.)
        boundary=adapter.project_ifc(model.to_string().encode())["boundaries"][0]
        self.assertEqual(boundary["roles"],["outer","hole"])

    def test_ifc_product_triangle_and_output_limits_reject(self):
        model, context = ifc_model()
        add_mesh(model,context)
        add_swept(model,context)
        for limit,value in (("MAX_IFC_PRODUCTS",1),("MAX_IFC_TRIANGLES",1),("MAX_OUTPUT_BYTES",100)):
            with self.subTest(limit=limit), patch.object(adapter,limit,value), self.assertRaises(ValueError):
                adapter.project_ifc(model.to_string().encode())

    def test_unavailable_representation_is_diagnosed(self):
        model, context = ifc_model()
        product = model.create_entity("IfcBuildingElementProxy",GlobalId=ifcopenshell.guid.new())
        result = adapter.project_ifc(model.to_string().encode())
        self.assertEqual(result["boundaries"],[])
        self.assertIn({"source_id":str(product.id()),"source_kind":"IfcBuildingElementProxy","code":"ifc_geometry_unavailable"},result["diagnostics"])

    def test_organizational_containers_do_not_report_missing_geometry(self):
        model, _ = ifc_model()
        for kind in ("IfcSite", "IfcBuilding", "IfcBuildingStorey"):
            model.create_entity(kind, GlobalId=ifcopenshell.guid.new())
        self.assertEqual(adapter.project_ifc(model.to_string().encode())["diagnostics"], [])

    def test_missing_length_units_do_not_invent_metre_geometry(self):
        model, context = ifc_model()
        add_swept(model, context)
        model.by_type("IfcProject")[0].UnitsInContext = None
        result = adapter.project_ifc(model.to_string().encode())
        self.assertEqual(result["boundaries"], [])
        self.assertEqual(result["diagnostics"][0]["code"], "ifc_length_units_unresolved")

    def test_open_section_is_diagnosed_without_hull_or_box_substitute(self):
        model,context=ifc_model()
        product=add_mesh(model,context)
        body=product.Representation.Representations[0].Items[0]
        body.CoordIndex=[(1,2,6),(1,6,5)]
        body.Closed=False
        result=adapter.project_ifc(model.to_string().encode())
        self.assertEqual(result["boundaries"],[])
        self.assertIn({"source_id":str(product.id()),"source_kind":"IfcBuildingElementProxy","code":"ifc_section_not_closed"},result["diagnostics"])


if __name__ == "__main__":
    unittest.main()
