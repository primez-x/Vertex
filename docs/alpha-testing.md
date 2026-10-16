# Vertex Windows x64 alpha: tester guide

Vertex is an offline-first Windows application for property measurement and
residential or light-commercial architectural work. This alpha is for testing
and feedback. It is **not production-qualified**, so do not use it as the only
record of production or client work.

## Before you start

- You need 64-bit (x64) Windows. You do not need Git, Visual Studio, Qt, or
  Python installed.
- Vertex does not need administrator rights. Do not run it as administrator.
- Test on copies. Keep a backup of any project you care about. Copy a real
  project in File Explorer before opening it. Alpha builds can have
  bugs that damage or lose project data.

## Install the alpha

1. Create a new, empty folder you can write to, such as `Documents\Vertex-alpha`.
   Do not use Program Files.
2. Extract the **entire** zip into that folder. Do not run Vertex from inside the
   zip viewer.
3. Keep all files together. Do not move `vertex.exe` or other files out of the
   folder.
4. Double-click `Start-Vertex.cmd` in the folder.

`SOURCE-BUNDLE.zip` contains the matching application source, dependency source
inputs, notices, and full setup bundle. Leave it compressed for normal testing;
its source paths can require a short extraction location or a long-path-capable
archive tool. `payload/help/user-guide.html` is the full offline user guide.
Use `Verify-Package.cmd` to check the runtime files. Settings and recovery data
use the normal Windows user profile; this package does not isolate those from
other Vertex copies.

The executable is unsigned. The supplied `.zip.sha256` file records the archive
checksum. This alpha has not been qualified on a clean Windows machine.

If Windows shows an unrecognized-app warning, confirm that the download came
from the source you were given before you run it.

## Find your way around

- **Measure** is for measured drawings and area totals. **Design** adds
  architectural views, the building model, and the 3D view. Switch workspaces
  from **View**.
- The left panel has **Layers** (active floor and layer), **Library** (objects,
  wall thickness and height), and **Details** (area measurements and GLA). If the
  panel is collapsed, reopen its tab from **View > Panels**.
- Menus: **File**, **Edit**, **Draw**, **Annotate**, **View**, **Output**,
  **Project**, **Help**.
- **Ctrl+K** opens Command search. Type a command name to run it.
- **Help > Keyboard shortcuts** lists the shortcuts.

## Test checklist

Mark each item pass, fail, or not tested. For any fail, note what you saw.

### Measure: wall room and GLA

- [ ] In Measure, keep **Wall** selected. Set thickness and height in **Library**,
  click the room's corners, then click its first corner to close the wall loop.
  A **Living area** should appear automatically.
- [ ] Open **Details**. Check the area and GLA. Facts supplied by a classification
  are marked "from classification". Unknown facts still need review.
- [ ] Press **Ctrl+Z** once. Record whether the Living area disappears with the
  closing wall, or whether a second Undo is needed. Press **Ctrl+Y** to redo.

### Save and reopen

- [ ] Save with **Ctrl+S**, close the project, and reopen it with **Ctrl+O**.
  Check that the rooms, furniture, openings, and measurements are unchanged.

### Area types

- [ ] Double-click the Living area to open **Properties**, then choose a different
  area type under **Classification**. If the area already has declared facts,
  change its category through **Edit facts** instead. Check the facts marked
  "from classification" and the GLA totals in **Details**.

### Automatic wall alignment and Shift

- [ ] Drag a single object (not a group) from **Library** or the plan toward a
  wall, close enough for it to attract. It should turn to match the wall's angle
  and settle against the wall's face. Test a square wall and a wall at an angle,
  such as about 15 degrees.
- [ ] Repeat the drag, but hold **Shift** before release. The object should keep
  its original orientation and not align to the wall.
- [ ] Drag the object out of the wall's range before release. It should return to
  its original orientation.
- [ ] Drag a multi-object selection near a wall. Its orientation should not change.
- [ ] Release an aligned object, then press **Ctrl+Z** once. The move and rotation
  should revert together. Press **Ctrl+Y** to redo. Save, reopen, and check that
  the aligned position and rotation are kept.
- [ ] Rotate a selected object with its rotation grip. Hold **Shift** for a fine
  angle, and check the live degree readout.

### Menus, search, and panels

- [ ] Open each menu: File, Edit, Draw, Annotate, View, Output, Project, Help.
  Note any command that is missing or named differently from this guide.
- [ ] Press **Ctrl+K** and search for "Edit sheet layout".
- [ ] Hide the left panel, then reopen **Layers**, **Library**, or **Details**
  from **View > Panels**.
- [ ] Open **Help > Keyboard shortcuts**.

### Design and native 3D

- [ ] Switch to **Design** (**Ctrl+2**). The 3D view is part of Design.
  Right-drag to orbit, middle-drag or Space + left-drag to pan, and scroll to zoom.
- [ ] Ctrl-click to add an object to a selection. Ctrl-drag left to right selects
  fully enclosed objects; Ctrl-drag right to left selects crossing objects.
- [ ] Check that the walls you drew appear in the 3D view.
- [ ] Double-click a placed 3D component. **Properties** should show **Size**,
  **Position**, and **Details**. Change one value only, and check that blank fields
  keep their values. **Reset** returns a value to the component type's default.

### PDF output

- [ ] Use **File > Export > Export sketch PDF**. Check the title block, room
  labels, and area totals on the Letter or A4 page.
- [ ] Optional sheet workflow: open **Output > Drawing sheets**, then **Add
  sheet…**. Adjust viewport scale and placement separately in **Output > Sheet
  layout manager…**. Sheet export stays disabled until the project has a sheet.
- [ ] Use **File > Export > Export selected sheet PDF**. Open the PDF and check
  that the whole room and its labels fit the sheet.
- [ ] Optional: **Output > Print selected sheet…** at actual size, without
  fitting to the printer page. Check that the scale is kept.

### References

- Portable launch: new PDF and image imports are unavailable. They require a
  protected runtime directory.
- [ ] After the optional protected setup below, use **File > Import > Reference image** with a PDF, PNG, JPEG, BMP, or TIFF
  file. Select the page if asked, and calibrate it from a known distance.
- [ ] Move and hide the reference. Save, reopen the project, and check that the
  reference is still aligned.

### CAD import (DXF and IFC)

- Portable launch: DXF and IFC imports are unavailable for the same protection
  requirement. After the optional protected setup, try a small test file and
  report the format, source application, result, and any error message.

### Optional protected setup for import testing

The full bundle retains the existing copy installer. It needs no administrator
rights when its destination is writable by you. It creates a separate runtime
whose module directories are read-only to the application user; do not change
those permissions to make an import work.

1. Extract `SOURCE-BUNDLE.zip` to a separate folder. Source paths can be long;
   use a short location or an archive tool with long-path support if needed.
2. Open Windows PowerShell in that extracted folder and run, using a new empty
   destination outside the extracted bundle:

   ```powershell
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install-offline-bundle.ps1 -InstallRoot "$env:LOCALAPPDATA\VertexAlpha\2026.10.16-alpha.1"
   ```

3. Start `bin\vertex.exe` from that destination, without administrator rights.
4. Keep the extracted bundle. To remove that protected copy, run its installer
   again with the same `-InstallRoot` and `-Action Uninstall`. Keep project files
   outside the runtime folder.

PDF export does not need this setup.

## Known limitations

- This is an alpha. It is not a production release, and it has not passed the
  production qualification gate.
- Resizing a selection that mixes object families still has known edge cases.
- Apex compatibility is not established. This alpha makes no Apex compatibility
  claim.
- New PDF/image/DXF/IFC imports need the optional protected setup. A plain
  extracted runtime intentionally refuses them; sandbox checks are unchanged.

## Sending feedback

Fill in the FEEDBACK template included in the package, and send it to the
project owner by the channel you were given. Use one report per problem.

```text
Build ID:
Windows version (Settings > System > About):
Steps to reproduce:
  1.
  2.
  3.
Expected result:
Actual result:
How often (always / sometimes / once):
Screenshot (optional):
Project file (optional):
```

- **Build ID:** copy it from the alpha manifest in the package folder.
- Before you send a screenshot or project file, remove personal data such as
  names, addresses, contact details, and account information. If a project
  contains client data, send a copy with placeholder content instead.

If Vertex closes unexpectedly, note the steps you took, the build ID, and whether
the project reopens. Keep your original project copy, and do not keep testing on
the only copy.
