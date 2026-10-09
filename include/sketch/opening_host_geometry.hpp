#pragma once

#include "sketch/document.hpp"
#include <TopoDS_Shape.hxx>
#include <cstddef>

namespace sketch {

// Actual manufactured door/window geometry in its resolved model frame.
// The complete source supplies phase activity, host elevation and sibling cuts.
// This is derived geometry; it never changes or substitutes persisted objects.
// Bare cuts have no manufactured body and refuse legacy host-copy geometry.
[[nodiscard]] TopoDS_Shape make_document_opening_host_shape(
    const DocumentSnapshot& source,const std::string& opening_id,
    std::size_t* cumulative_native_work=nullptr);

} // namespace sketch
