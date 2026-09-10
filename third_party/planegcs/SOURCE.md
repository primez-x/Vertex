# PlaneGCS source provenance

This directory contains a source-only extraction from the FreeCAD `1.1.3` tag,
commit `145529fe741292ff0b3977a01195bf0247425794`:

<https://github.com/FreeCAD/FreeCAD/tree/145529fe741292ff0b3977a01195bf0247425794/src/Mod/Sketcher/App/planegcs>

The unmodified upstream files are the PlaneGCS `GCS`, `Geo`, `Constraints`,
`SubSystem`, `Util`, and `qp_eq` headers and sources, plus
`src/boost_graph_adjacency_list.hpp` and the repository `LICENSE`. They were
extracted with `git archive` from that exact commit. The four compatibility
headers under `upstream/src/Base`, `upstream/src/FCConfig.h`, and
`upstream/src/Mod/Sketcher/SketcherGlobal.h` are local, narrowly scoped shims;
they are not represented as upstream FreeCAD files.

SHA-256 of the unmodified extracted files:

```text
7ffe1954587c77dfba1cf8eb9b2ea743671fa6e63f9e7a2f258119d42e14eefe  upstream/LICENSE
e3eec1b131456c2a8a7981425bb3b5707260c6392e2f513946ceaff8a847c738  upstream/src/boost_graph_adjacency_list.hpp
cb709b3f179c7540f749e3723669a652877156a1ec5d10d7e97ad0fd7f4a812b  upstream/src/Mod/Sketcher/App/planegcs/Constraints.cpp
2b32626a5d9976ef1b14343692faec55058875433f6982523504ca3d77f76a15  upstream/src/Mod/Sketcher/App/planegcs/Constraints.h
501f0d279ea57e39b61f49ca433e05c54fff3a0a7bfe6fe657b0f83f3464355c  upstream/src/Mod/Sketcher/App/planegcs/GCS.cpp
79a0683acd84273efd8fb2dce36d2dac23011b10b2c693ec5615ce9eb786f18a  upstream/src/Mod/Sketcher/App/planegcs/GCS.h
166a01743bba5e98640bbdbfa5fa615557dd2e9b507f7450de1e6a4649db8ed9  upstream/src/Mod/Sketcher/App/planegcs/Geo.cpp
17c59287625820418ad66c3e6cf9463ad9388f169de11535d2c25fa428771712  upstream/src/Mod/Sketcher/App/planegcs/Geo.h
28aaf79ce58d4515a6aa7d2c79c702d83b09e7633b7575706b24974b4953ecfb  upstream/src/Mod/Sketcher/App/planegcs/qp_eq.cpp
e1083df385b8639fc3fe609dc60f81bd662a99b10e4efbaab9b50c6d9026afab  upstream/src/Mod/Sketcher/App/planegcs/qp_eq.h
c48700843911f923d1c05bbda736e45df66826db5a1860aca19aa16cf035bc2a  upstream/src/Mod/Sketcher/App/planegcs/SubSystem.cpp
9a601de5fc441675551cf717a21da184060eb2f95bd76a026430f053401ae758  upstream/src/Mod/Sketcher/App/planegcs/SubSystem.h
ff6df589332423ecb4b29e7b2f8d9f2d779f6520861b4155bb8e5a9b2f7a4eeb  upstream/src/Mod/Sketcher/App/planegcs/Util.h
```
