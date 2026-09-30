"""Generate real binary/foreign fixtures for the independent worker runner."""
import importlib.util
import pathlib
import sys

spec = importlib.util.spec_from_file_location(
    "cad_adapter_tests", pathlib.Path(__file__).with_name("test_cad_library_adapter.py"))
fixtures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)
output = pathlib.Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)
doc = fixtures.ezdxf.new("R2013")
doc.header["$INSUNITS"] = 6
leaf = doc.blocks.new("LEAF")
leaf.add_line((0, 0), (1, 0))
parent = doc.blocks.new("PARENT")
parent.add_blockref("LEAF", (2, 0))
doc.modelspace().add_blockref("PARENT", (10, 20))
(output / "nested-binary.dxf").write_bytes(fixtures.dxf_bytes(doc, binary=True))
model, context = fixtures.ifc_model()
fixtures.add_swept(model, context, hollow=True)
(output / "hollow-rotated.ifc").write_text(model.to_string(), encoding="utf-8")
model, context = fixtures.ifc_model()
fixtures.add_mesh(model, context)
(output / "disconnected.ifc").write_text(model.to_string(), encoding="utf-8")
model, context = fixtures.ifc_model()
body = fixtures.add_swept(model, context, transform=False)
body.Representation.Representations[0].Items[0].SweptArea.YDim = 4.
points = [model.create_entity("IfcCartesianPoint", point) for point in
          [(-2., -2.), (2., -2.), (2., 2.), (-2., 2.), (-2., -2.)]]
body.Representation.Representations[0].Items[0].SweptArea = model.create_entity(
    "IfcArbitraryClosedProfileDef", "AREA", None, model.create_entity("IfcPolyline", points))
slab = model.create_entity("IfcSlab", GlobalId=fixtures.ifcopenshell.guid.new(),
    ObjectPlacement=body.ObjectPlacement, Representation=body.Representation, PredefinedType="FLOOR")
model.remove(body)
body = fixtures.add_swept(model, context, transform=False)
profile = body.Representation.Representations[0].Items[0].SweptArea
profile.XDim = 2.
profile.YDim = 2.
opening = model.create_entity("IfcOpeningElement", GlobalId=fixtures.ifcopenshell.guid.new(),
    ObjectPlacement=body.ObjectPlacement, Representation=body.Representation)
model.remove(body)
model.create_entity("IfcRelVoidsElement", GlobalId=fixtures.ifcopenshell.guid.new(),
    RelatingBuildingElement=slab, RelatedOpeningElement=opening)
(output / "slab-void.ifc").write_text(model.to_string(), encoding="utf-8")
model.by_type("IfcProject")[0].UnitsInContext = None
(output / "units-unresolved.ifc").write_text(model.to_string(), encoding="utf-8")
