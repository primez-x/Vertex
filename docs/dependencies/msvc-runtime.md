# MSVC app-local runtime evidence

Status as of 2026-10-06: the selected x64 CRT bytes match an inspected Microsoft Visual Studio Build Tools redist directory and the files fall within the documented Visual Studio 2022 redist path. Redistribution clearance remains unqualified. The existing cache manifest still records `licensing_clearance: false` and `version_verification: operator-declared`; this document does not change those declarations or decide whether any particular person or organization held the required license.

## Selected payload

The app-local payload is exactly these five nondebug DLLs. Windows file and product version resources both report `14.44.35211.0` for every file.

| File | Bytes | File / product version | SHA-256 |
| --- | ---: | --- | --- |
| `msvcp140.dll` | 557,728 | `14.44.35211.0` | `0f885b509a685d2bbfa652fed26b5fb31d88fbdab0a978c641d1c7b8aa460aa9` |
| `msvcp140_1.dll` | 35,952 | `14.44.35211.0` | `bfad5aef4c63a669e3c140655cdfdf395b6c979b400a447bd5dcb65ed8826c3d` |
| `msvcp140_2.dll` | 280,200 | `14.44.35211.0` | `3ea06f0ee098b4823cb79599df3780e7f23cce52c19aac31d2a0d47efe33a5e9` |
| `vcruntime140.dll` | 124,544 | `14.44.35211.0` | `d5e4d9a3e835fa679450145d6a7d94e36573a509317111904d9b3712c30d9066` |
| `vcruntime140_1.dll` | 49,792 | `14.44.35211.0` | `1f2d41c4aa5db0bc33ebf7b66d72943a817d7ce6cbe880502a9403823633093f` |

Each staged hash matches the corresponding local source file under `VC/Redist/MSVC/14.44.35112/x64/Microsoft.VC143.CRT`. The `14.44.35112` directory label and the DLLs' `14.44.35211.0` version resources describe different observed values; do not treat them as interchangeable. This is a byte comparison to the inspected local directory, not an independent Microsoft signature or licensing check. The source is in the regular x64 CRT redist tree; no `debug_nonredist` path or debug DLL is included.

The retained `Redist.txt` is a 187-byte pointer to Microsoft's current Visual Studio 2022 Distributable Code page. The retained `ThirdPartyNotices.txt` is 7,043,986 bytes; its content was not opened as part of this review. These notice files are evidence inputs, not proof of redistribution rights.

## Applicable Microsoft terms located

Microsoft's [Visual Studio 2022 redistribution list](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution) identifies the list as the “Distributable List” referenced by the Visual Studio Enterprise, Professional, and Community 2022 license terms. It says that a validly licensed copy of one of those products may copy and distribute listed code with a program, subject to its license terms. For Visual C++ runtime files, the page permits unmodified files under `VC/Redist` and its subfolders, except the `debug_nonredist` and `onecore/debug_nonredist` folders. It also says distribution of the runtime package, merge modules, and individual binaries obtained from the linked download pages is limited to licensed Visual Studio users. The observed `x64/Microsoft.VC143.CRT` source directory is inside the included redist tree and outside the named debug exclusions; the matching local hashes support that the staged DLL bytes were not modified.

The [Visual Studio Community 2022 license](https://visualstudio.microsoft.com/license-terms/vs2022-ga-community/) permits an individual working on their own applications to use the software to develop and test those applications. For organizations, it permits any number of users to develop and test applications released under an Open Source Initiative (OSI)-approved open-source license, along with several other listed categories. Organizations outside those categories may use up to five users concurrently only if they are not an “enterprise”; the license defines an enterprise by more than 250 PCs or users, or more than US$1 million (or the equivalent in another currency) in annual revenue across the organization and its affiliates. Its distributable-code section requires adding significant primary functionality and requiring distributors and external end users to accept terms that protect the Microsoft code at least as much as the agreement. It also excludes preview, prerelease, and beta runtimes and restricts use of Microsoft's marks and placing the code under an excluded license.

The [Diagnostic Build Tools for Visual Studio 2022 license](https://visualstudio.microsoft.com/license-terms/vs2022-ga-diagnosticbuildtools/) calls that software a supplement to Visual Studio Community, Professional, and Enterprise and requires a valid license to one of those Visual Studio products for the ordinary use rights. Its separate no-Visual-Studio-license allowance is narrow: Build Tools may compile and build third-party C++ components released under an OSI-approved license when reasonably required to build the user's application. The agreement bars using that allowance to develop or test those dependencies, except for minor changes needed to make them compile and build; a separate Build Devices clause permits verifying dependencies and running their quality or performance tests as part of the application build process. The workflow must fit those terms. An open-source license on the application does not, by itself, turn this dependency allowance into a license to build the application or to redistribute its CRT files.

Microsoft's [Visual Studio licensing guidance](https://www.microsoft.com/licensing/guidance/Visual-Studio) likewise describes Build Tools as a supplement and summarizes the OSI C++ dependency condition. Microsoft labels that page informational and says it is not the agreement; the applicable product license controls. The separately titled [Supplement for Microsoft Visual Studio 2022](https://visualstudio.microsoft.com/license-terms/vs2022-ga-supplemental/) is a different supplement, and its own terms also require a valid Visual Studio 2022 license. It is not the Build Tools license.

The separately titled [Visual C++ 2015–2022 Runtime license](https://visualstudio.microsoft.com/license-terms/vs2022-cruntime/) grants installation and use rights for that runtime software. It is not the distributable-code grant relied on for shipping these app-local DLLs; that grant is in the Visual Studio license terms and REDIST list above.

## Cached source bytes and remaining basis

The four official DOCX files below were fetched unchanged from the Microsoft URLs linked by the [Visual Studio license directory](https://visualstudio.microsoft.com/license-terms/). HTTP status, content type, exact byte count, SHA-256, final URL, and retrieval time are in the private receipt `artifacts/reset-delivery/msvc-terms-20261006-01/acquisition.json`. The inert DOCX files are cached under `.deps/downloads/msvc-terms/` and were read only by extracting their Word XML text.

| Microsoft document | Cached file | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| Visual Studio Community 2022 | `Visual-Studio-2022-Community-License-EN.docx` | 62,859 | `41a207b10c8ab91d0d2f10a854715f73dca54509581692d2fe179aa3ffcb8540` |
| Diagnostic Build Tools for Visual Studio 2022 (March 2024 update) | `Visual-Studio-2022-Diagnostic-Build-Tools-Agent-License_Update-March-2024_EN.docx` | 34,395 | `2f66b86a00e8d9833789897ce23d05a4a2dbea370cf39c8c1098dbc17d0e7bdc` |
| Supplement for Microsoft Visual Studio 2022 | `Visual-Studio-2022-Supplement-License-EN.docx` | 30,167 | `2ca53f102483caf4cd835742d4c7d0ee83093aed2c14a5ccada171821ca7c785` |
| Visual C++ Runtime 2015–2022 | `Visual-C-Runtime-2015-2022-License-1.docx` | 39,644 | `f1e3d56ceb2ad68aae0711b910375009e651ac5530fa0760f0dea6e81e54fae1` |

No eligible license holder or accepted installer license was identified in the narrow local installer state metadata inspected; it contained installation-version metadata but no EULA, license, acceptance, or terms field. This bounded check does not establish that a separate license record does not exist, and installed files alone do not establish eligibility.

Before setting `licensing_clearance` to true, record evidence for all of the following:

1. Identify the distributor and the developer or build user whose Visual Studio license supplies the right to distribute this runtime. Record the exact product, edition, applicable agreement, and period covering the relevant build and distribution. If relying on Community, establish whether individual-use or organizational terms apply and, for an organization, whether the relevant use is within an OSI-approved or other listed category and whether the enterprise restriction applies.
2. Confirm the actual Visual Studio Build Tools use fits the relevant product terms. A Build Tools installation, the presence of `Redist.txt`, or Vertex's open-source release alone does not prove that basis. The no-Visual-Studio-license C++ allowance is only for qualifying dependencies, not general application development or CRT redistribution.
3. Confirm the release includes only unmodified, nondebug files from the listed redist tree and that the application, distributor, and end-user terms meet the applicable Visual Studio distributable-code requirements.

Until that basis is documented and reviewed, keep the existing manifest's `licensing_clearance: false` and its operator-declared version fields unchanged.
