"""Exercise the bundled CAD SDK with generated developer fixtures, offline.

This does not import user projects, certify sandbox admission, or mark the
runtime as redistributable. The application worker remains the import boundary.
"""
import argparse
import json
import os
import pathlib
import subprocess
import tempfile

from inspect_selected_cad_runtime import inspect as inspect_runtime

ROOT = pathlib.Path(__file__).resolve().parents[1]
PROBE = r'''
import io,json,pathlib,sys
import ifcopenshell,ifcopenshell.geom,ifcopenshell.guid
import ezdxf
from ezdxf.document import Drawing
from ezdxf.lldxf.tagger import binary_tags_loader
assert sys.flags.isolated and sys.flags.ignore_environment and sys.flags.dont_write_bytecode
root=pathlib.Path(sys.executable).resolve().parent
for module in (ifcopenshell,ezdxf):
    assert pathlib.Path(module.__file__).resolve().is_relative_to(root)
doc=ezdxf.new('R2013'); doc.modelspace().add_line((0,0),(1,0))
stream=io.BytesIO(); doc.write(stream,fmt='bin')
binary=Drawing.load(binary_tags_loader(stream.getvalue(),errors='strict'))
assert len(binary.modelspace())==1 and binary.modelspace()[0].dxftype()=='LINE'
assert tuple(binary.modelspace()[0].dxf.end)==(1,0,0)
inner=doc.blocks.new('INNER'); inner.add_line((0,0),(1,0))
outer=doc.blocks.new('OUTER'); outer.add_blockref('INNER',(2,0))
placed=doc.modelspace().add_blockref('OUTER',(10,0))
leaf=list(list(placed.virtual_entities())[0].virtual_entities())[0]
assert tuple(leaf.dxf.start)==(12,0,0) and tuple(leaf.dxf.end)==(13,0,0)
m=ifcopenshell.file(schema='IFC4')
origin=m.create_entity('IfcCartesianPoint',(0.,0.,0.))
axis=m.create_entity('IfcAxis2Placement3D',origin,None,None)
context=m.create_entity('IfcGeometricRepresentationContext',None,'Model',3,1e-5,axis,None)
vertices=[(0.,0.,0.),(1.,0.,0.),(1.,1.,0.),(0.,1.,0.),(0.,0.,1.),(1.,0.,1.),(1.,1.,1.),(0.,1.,1.)]
triangles=[(1,3,2),(1,4,3),(5,6,7),(5,7,8),(1,2,6),(1,6,5),(2,3,7),(2,7,6),(3,4,8),(3,8,7),(4,1,5),(4,5,8)]
points=m.create_entity('IfcCartesianPointList3D',vertices)
body=m.create_entity('IfcTriangulatedFaceSet',points,None,True,triangles,None)
rep=m.create_entity('IfcShapeRepresentation',context,'Body','Tessellation',[body])
shape=m.create_entity('IfcProductDefinitionShape',None,None,[rep])
m.create_entity('IfcWall',GlobalId=ifcopenshell.guid.new(),Representation=shape)
parsed=ifcopenshell.file.from_string(m.to_string())
settings=ifcopenshell.geom.settings(); settings.set('use-world-coords',True); settings.set('convert-back-units',False)
created=ifcopenshell.geom.create_shape(settings,parsed.by_type('IfcWall')[0])
geometry=created.geometry
assert len(geometry.faces)>=36
coords=geometry.verts
assert all(min(coords[i::3])==0 and max(coords[i::3])==1 for i in range(3))
print(json.dumps(dict(python=sys.version.split()[0],ifcopenshell=ifcopenshell.version,ezdxf=ezdxf.__version__,
    isolated=True,binary_dxf_line=True,nested_insert_world_coordinates=True,ifc_cube_world_geometry=True)))
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-root",type=pathlib.Path,default=ROOT/".deps/cad-runtime/3.13.15")
    parser.add_argument("--output",type=pathlib.Path)
    args=parser.parse_args()
    runtime=args.runtime_root.resolve()
    lock=json.loads((ROOT/"third_party/cad-runtime-lock.json").read_text())
    identity=inspect_runtime(runtime,ROOT)
    with tempfile.TemporaryDirectory(prefix="vertex-cad-probe-") as directory:
        poison=pathlib.Path(directory)
        (poison/"ezdxf.py").write_text("raise RuntimeError('untrusted PYTHONPATH module loaded')")
        (poison/"sitecustomize.py").write_text("raise RuntimeError('untrusted site customization loaded')")
        system=os.environ.get("SystemRoot",r"C:\Windows")
        environment={"SystemRoot":system,"WINDIR":system,
            "PATH":str(runtime)+os.pathsep+str(pathlib.Path(system)/"System32"),
            "TEMP":directory,"TMP":directory,"APPDATA":directory,"LOCALAPPDATA":directory,
            "USERPROFILE":directory,"XDG_CONFIG_HOME":directory,
            "PYTHONPATH":directory,"PYTHONHOME":directory}
        result=subprocess.run([str(runtime/"python.exe"),"-I","-B","-c",PROBE],cwd=directory,
            env=environment,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=60,
            creationflags=getattr(subprocess,"CREATE_NO_WINDOW",0))
        if result.returncode:
            raise RuntimeError(f"Bundled SDK probe failed ({result.returncode}): {result.stderr.decode('utf-8',errors='replace')}")
        report=json.loads(result.stdout)
    expected_ifc=("0.0.0" if identity["kind"]=="controlled" else lock["library_versions"]["ifcopenshell"])
    if (report["python"]!=lock["python_version"] or
            report["ezdxf"]!=lock["library_versions"]["ezdxf"] or report["ifcopenshell"]!=expected_ifc):
        raise ValueError("Bundled SDK version differs from lock")
    if inspect_runtime(runtime,ROOT)!=identity:
        raise ValueError("Selected SDK changed during its generated fixture probe")
    report.update(runtime_kind=identity["kind"],manifest_sha256=identity["manifest_sha256"],
                  ifc_extension_sha256=identity["ifc_extension_sha256"],
                  ifc_wrapper_sha256=identity["ifc_wrapper_sha256"])
    if identity["kind"]=="controlled":
        report["source_revision"]=identity["source_revision"]
        report["ifcopenshell_version_informative_only"]=True
    report.update(production_worker_integrated=False,qualification="incomplete")
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(report))


if __name__=="__main__": main()
