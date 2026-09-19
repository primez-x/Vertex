// Compatibility shim for the extracted PlaneGCS target.
#pragma once

#if defined(_WIN32)
# if defined(VERTEX_PLANEGCS_BUILD)
#  define SketcherExport __declspec(dllexport)
# else
#  define SketcherExport __declspec(dllimport)
# endif
#else
# define SketcherExport __attribute__((visibility("default")))
#endif
