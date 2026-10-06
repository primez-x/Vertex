#include "sketch/assembly_geometry.hpp"
#include "sketch/architecture.hpp"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <cmath>
#include <stdexcept>
#include <string>

namespace sketch {

AssemblyGeometry make_assembly_geometry(const AssemblyExpansion& expansion) {
    AssemblyGeometry result;
    result.solids.reserve(expansion.profiles.size());
    if (expansion.profiles.empty()) return result;
    try {
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        for (const auto& source : expansion.profiles) {
            const auto& placement = source.transform;
            const auto& offset = placement.translation_m;
            if (!std::isfinite(placement.scale) || placement.scale <= 0 ||
                !std::isfinite(placement.rotation_radians) ||
                !std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z)) {
                throw std::invalid_argument("Assembly solid transform must be finite with positive scale");
            }
            const auto& profile = source.profile;
            const auto local = make_slab(Slab{profile.id, profile.outer, profile.holes,
                profile.height_m, profile.elevation_m});
            gp_Trsf scale, rotation, translation;
            scale.SetScale(gp_Pnt(0, 0, 0), placement.scale);
            rotation.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)),
                placement.rotation_radians);
            translation.SetTranslation(gp_Vec(offset.x, offset.y, offset.z));
            // OCCT multiplication applies the rightmost transform first.
            const gp_Trsf transform = translation * rotation * scale;
            BRepBuilderAPI_Transform transformed(local, transform, true);
            if (!transformed.IsDone() || transformed.Shape().IsNull() ||
                !BRepCheck_Analyzer(transformed.Shape()).IsValid()) {
                throw std::invalid_argument("Assembly profile did not produce a valid transformed solid");
            }
            const double volume = solid_volume(transformed.Shape());
            if (!std::isfinite(volume) || volume <= 0) {
                throw std::invalid_argument("Assembly solid volume must be finite and positive");
            }
            result.volume_m3 += volume;
            if (!std::isfinite(result.volume_m3)) {
                throw std::invalid_argument("Assembly solid volume exceeds the supported numeric range");
            }
            result.solids.push_back({source, transformed.Shape(), volume});
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
