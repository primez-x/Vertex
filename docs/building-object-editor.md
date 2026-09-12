# Building object editor

`sketch::desktop::BuildingObjectDialog` is the Qt Widgets authoring seam for
the six bounded architectural solids.  It owns no `Document`: construction
and editing collect controls, validate the complete semantic object through
the building entity codec, and return one candidate `Entity` only after a
successful `submit()`.

```cpp
sketch::desktop::BuildingObjectDialog dialog(
    original_entity,        // std::nullopt for creation
    metric_units,
    parent_widget);
if (dialog.exec() == QDialog::Accepted) {
    if (const auto candidate = dialog.candidate()) {
        // Apply one command against the revision captured before exec().
    }
}
```

The same parser is used by the Submit button and the public `submit()` method.
Lengths and coordinates use `parse_quantity` with metres as the metric default
or feet as the imperial default, while explicit suffixes such as `900 mm`,
`2.5 m`, `3 ft`, and `6 in` remain accepted in either mode.  Orientation and
rotation controls are entered in degrees and stored as radians.  Riser count
is a positive integer from 1 through 10000.  Invalid input sets
`buildingObjectError`, keeps the dialog open, and leaves `candidate()` empty;
no message box or document mutation is involved.

## Quantity provenance

Accepted quantity fields carry their entered expression in the reserved
`properties.quantity_entries` object. Its keys are JSON pointers to the
canonical metre value, and each value has this versioned shape:

```json
{
  "version": 1,
  "original_expression": "1/3 ft",
  "entered_unit": "ft",
  "exact_metres": {
    "numerator": 127,
    "denominator": 1250
  }
}
```

The supported `entered_unit` names are `m`, `mm`, `cm`, `ft`, and `in`.
Typical keys include `/width_m`, `/base_center_m/0`, `/start_m/2`, and
`/top_landing/depth_m`. The rational pair is the exact, normalized metre
value returned by `parse_quantity`; the canonical numeric property remains
authoritative.

When editing, the dialog shows an original expression again only when the
receipt version, types, unit, expression, rational pair, and parsed canonical
double all agree. An edited field receives a new receipt. Unchanged receipt
records, including stale or future-version records, are retained as opaque
metadata; they never override the canonical geometry and are not shown unless
their full contract validates. A receipt is removed when its canonical field
is edited or removed, and only quantity fields active in the current form are
created. Switching display units therefore preserves a valid expression
without converting the stored geometry. A suffixless expression gets its
recorded unit appended when the dialog's default unit differs, so the field
remains unambiguous.

Legacy or programmatically constructed entities do not need receipts. Their
canonical values are shown normally; a receipt is added only for a quantity
field that the user submits through this dialog. Angles, vectors such as a
beam's up direction, and integer riser counts have no quantity receipt. The
`quantity_entries` name is reserved for this provenance map; unrelated entity
properties and extension metadata remain caller-owned and are preserved by
edits.

The creation controls have these stable object names:

| Control | Object name |
| --- | --- |
| Entity type selector | `buildingObjectType` |
| Form selector | `buildingObjectForm` |
| Inline error label | `buildingObjectError` |
| Submit button | `buildingObjectSubmit` |
| Scroll container | `buildingObjectScrollArea` |
| Current form stack | `buildingObjectFormStack` |

The current form page uses shared names so tests and callers can select a form
then locate its controls without depending on a page implementation:

| Semantic value | Object name |
| --- | --- |
| Base X/Y/Z | `buildingObjectBaseX`, `buildingObjectBaseY`, `buildingObjectBaseZ` |
| Beam start X/Y/Z | `buildingObjectStartX`, `buildingObjectStartY`, `buildingObjectStartZ` |
| Beam end X/Y/Z | `buildingObjectEndX`, `buildingObjectEndY`, `buildingObjectEndZ` |
| Beam up X/Y/Z | `buildingObjectUpX`, `buildingObjectUpY`, `buildingObjectUpZ` |
| Orientation or rotation in degrees | `buildingObjectOrientationDegrees` |
| Width, depth, height | `buildingObjectWidth`, `buildingObjectDepth`, `buildingObjectHeight` |
| Circular radius | `buildingObjectRadius` |
| Stair risers, rise, going | `buildingObjectRiserCount`, `buildingObjectTotalRise`, `buildingObjectGoing` |
| Top landing toggle | `buildingObjectLandingEnabled` |
| Top landing depth/thickness | `buildingObjectLandingDepth`, `buildingObjectLandingThickness` |
| Roof run/span/rise | `buildingObjectRun`, `buildingObjectSpan`, `buildingObjectRise` |
| Gable length | `buildingObjectLength` |
| Roof overhang/thickness | `buildingObjectOverhang`, `buildingObjectThickness` |
| Derived roof pitch label | `buildingObjectDerivedPitch` |

Roof pitch is derived from the entered rise and horizontal run.  A sloped panel
uses `rise / run`; a gable uses `rise / (span / 2)`.  A sloped panel may use an
exactly zero rise to author a flat roof; the derived pitch then displays `0°`.
There is no editable pitch control, so the dialog cannot submit an inconsistent
pitch/rise pair.  Gable roofs still require a positive rise and pitch.

For editing, the constructor first decodes and validates the original entity.
Malformed or unsupported originals show an inline error and disable Submit.
Valid edits preserve the original ID, `required` flag, extension metadata, and
unknown properties.  Canonical fields are merged into a copy of the original
properties.  Populated values track edits so changing one field does not
rewrite untouched high-precision coordinates, angles, vectors, or roof pitch.
The resulting candidate is still only a value; the caller owns the expected
revision and atomic `Document` command.

The dialog is a bounded authoring surface.  It does not provide structural,
code-compliance, material assembly, multi-flight stair, or production-complete
roof design checks beyond the existing solid builders.  The test executable
uses Qt's noninteractive error guard and keeps normal runs headless.  For
selected visual review, pass:

```text
building_object_dialog_tests.exe --capture-directory C:\temp\building-dialog-captures
```

The option writes six valid-form PNGs, an imperial-default stair state, a unit
switch state, and one invalid-input state.  The normal
CTest invocation writes no captures.

`scripts/test-building-editor.ps1` runs those captures in hidden Windows
processes at scales 1 and 1.5, with a 15-second timeout and image/executable
hashes. Dimension fields use short fixed decimal text; untouched edit fields
retain their exact stored values. Generated imperial defaults use concise feet
text only when it round-trips exactly; otherwise they retain an explicit SI
value. User-entered quantity expressions remain verbatim regardless of length.
Calculated pitch is displayed to three decimal places without changing its
stored precision.
