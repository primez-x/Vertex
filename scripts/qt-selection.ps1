# Shared public upstream Qt SDK selection; never discovers another installed SDK.
function Get-VertexQtSelection {
    param([Parameter(Mandatory = $true)][string]$ProjectRoot)
    $selection = Get-Content -LiteralPath (Join-Path $ProjectRoot 'third_party/qt-sdk.json') -Raw | ConvertFrom-Json
    if ($selection.schema_version -ne 1 -or $selection.version -notmatch '^6\.\d+\.\d+$' -or
        $selection.architecture -ne 'win64_msvc2022_64' -or $selection.directory -ne 'msvc2022_64' -or
        $selection.aqt_revision -notmatch '^[0-9a-f]{40}$' -or
        $selection.aqt_source_archive.url -ne "https://codeload.github.com/miurahr/aqtinstall/zip/$($selection.aqt_revision)" -or
        $selection.aqt_source_archive.sha256 -notmatch '^[0-9a-f]{64}$' -or
        ($selection.archives -join ',') -ne 'qtbase,qtsvg' -or ($selection.modules -join ',') -ne 'qtpdf' -or
        ($selection.components -join ',') -ne 'Core,Gui,Widgets,OpenGL,OpenGLWidgets,PrintSupport,Pdf,Svg') {
        throw 'Invalid pinned Qt SDK selection.'
    }
    $selection | Add-Member -NotePropertyName prefix -NotePropertyValue (Join-Path $ProjectRoot ".deps/qt/$($selection.version)/$($selection.directory)")
    return $selection
}
