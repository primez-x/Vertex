#include "sketch/assembly_geometry.hpp"
#include "sketch/architecture.hpp"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Mat.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <gp_XYZ.hxx>
#include <cmath>
#include <stdexcept>
#include <string>

namespace sketch {

TopoDS_Shape transform_assembly_shape(const TopoDS_Shape& source,const AssemblyTransform& placement) {
    const auto& offset=placement.translation_m;
    const auto scale_z=placement.scale*placement.vertical_scale;
    if(!std::isfinite(placement.scale) || placement.scale<=0 ||
        !std::isfinite(placement.vertical_scale) || placement.vertical_scale<=0 ||
        !std::isfinite(scale_z) || scale_z<=0 || !std::isfinite(placement.rotation_radians) ||
        !std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z)) {
        throw std::invalid_argument("Assembly solid transform must be finite with positive XY and Z scales");
    }
    try {
        if(source.IsNull() || !BRepCheck_Analyzer(source).IsValid())
            throw std::invalid_argument("Assembly solid transform requires a valid source shape");
        TopoDS_Shape result;
        if(placement.vertical_scale==1) {
            gp_Trsf mirror, scale, rotation, translation;
            if(placement.mirrored_y)
                mirror.SetMirror(gp_Ax2(gp_Pnt(0,0,0),gp_Dir(0,1,0)));
            scale.SetScale(gp_Pnt(0,0,0),placement.scale);
            rotation.SetRotation(gp_Ax1(gp_Pnt(0,0,0),gp_Dir(0,0,1)),placement.rotation_radians);
            translation.SetTranslation(gp_Vec(offset.x,offset.y,offset.z));
            // OCCT multiplication applies the rightmost transform first.
            BRepBuilderAPI_Transform transformed(source,translation*rotation*scale*mirror,true);
            if(!transformed.IsDone())throw std::invalid_argument("Assembly solid transform failed");
            result=transformed.Shape();
        } else {
            const auto c=std::cos(placement.rotation_radians),s=std::sin(placement.rotation_radians);
            const auto parity=placement.mirrored_y ? -1.0:1.0;
            const gp_Mat linear(placement.scale*c,-placement.scale*s*parity,0,
                placement.scale*s,placement.scale*c*parity,0,0,0,scale_z);
            const gp_GTrsf affine(linear,gp_XYZ(offset.x,offset.y,offset.z));
            BRepBuilderAPI_GTransform transformed(source,affine,true);
            if(!transformed.IsDone())throw std::invalid_argument("Assembly affine solid transform failed");
            result=transformed.Shape();
        }
        if(result.IsNull() || !BRepCheck_Analyzer(result).IsValid())
            throw std::invalid_argument("Assembly transform did not produce a valid native shape");
        const auto volume=solid_volume(result);
        if(!std::isfinite(volume) || volume<=0)
            throw std::invalid_argument("Assembly transformed body volume must be finite and positive");
        return result;
    } catch(const Standard_Failure& error) {
        throw std::invalid_argument(std::string("Assembly solid transform failed: ")+error.what());
    }
}

AssemblyGeometry make_assembly_geometry(const AssemblyExpansion& expansion) {
    AssemblyGeometry result;
    result.solids.reserve(expansion.profiles.size());
    if (expansion.profiles.empty()) return result;
    try {
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        for (const auto& source : expansion.profiles) {
            const auto& profile = source.profile;
            const auto local = make_slab(Slab{profile.id, profile.outer, profile.holes,
                profile.height_m, profile.elevation_m});
            const auto transformed=transform_assembly_shape(local,source.transform);
            const double volume = solid_volume(transformed);
            if (!std::isfinite(volume) || volume <= 0) {
                throw std::invalid_argument("Assembly solid volume must be finite and positive");
            }
            result.volume_m3 += volume;
            if (!std::isfinite(result.volume_m3)) {
                throw std::invalid_argument("Assembly solid volume exceeds the supported numeric range");
            }
            result.solids.push_back({source, transformed, volume});
            builder.Add(compound, result.solids.back().shape);
        }
        if (!BRepCheck_Analyzer(compound).IsValid()) {
            throw std::invalid_argument("Assembly solids did not produce a valid compound");
        }
        result.shape = compound;
        return result;
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string("Assembly geometry failed: ") + error.what());
    }
}

} // namespace sketch
