# Terrain surfaces

The Windows application stores a terrain surface as a versioned, offline `terrain_surface`
entity. The model is a bounded triangulated irregular network (TIN), so the same authored
points and triangles drive plan presentation, contour lines, elevation/section projection,
and the native OCCT surface. No network service or hosted elevation source is required.

## Persisted model

`properties.model` has exactly these version-1 fields:

```json
{
  "version": 1,
  "provenance": "selected boundary",
  "points": [
    {"id": "terrain-center", "x_m": 5.0, "y_m": 5.0, "elevation_m": 101.5},
    {"id": "terrain-p0", "x_m": 0.0, "y_m": 0.0, "elevation_m": 100.0}
  ],
  "triangles": [[0, 1, 2]],
  "contour_interval_m": 1.0,
  "visible": true
}
```

Coordinates and elevations are metres. Point identifiers are stable within the model.
The document boundary limits a surface to 4,096 points, 8,192 triangles, and 1,000,000
derived contour segments. Every triangle must have three distinct in-range indices, a
non-zero plan footprint, and no undirected edge may be shared by more than two triangles.

The current authoring command is **Create terrain surface from selected boundary**. It accepts
one elevation quantity per straight vertex of a closed convex boundary. The command creates a
centroid fan, validates the native faces before mutation, and stores the source boundary ID
and original elevation expressions as inspectable metadata. The original boundary remains a
separate measurement or room object.

Plan edges are deduplicated triangle edges. Contours use linear interpolation at the configured
interval and omit a level that only touches one triangle vertex, avoiding duplicate slivers.
Elevation and section views project the same native face compound used by the 3D viewport.

This slice does not claim a complete site model, georeferencing, import of Apex terrain files,
survey-device exchange, or production compatibility certification. Those remain explicit gaps
in the requirements ledger.
