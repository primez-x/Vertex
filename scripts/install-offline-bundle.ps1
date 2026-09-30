[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$InstallRoot,

    [ValidateSet('Install', 'Repair', 'Uninstall')]
    [string]$Action = 'Install'
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    throw "offline bundle installation: $Message"
}

function Assert-NoReparseChain([string]$RootPath, [string]$Candidate, [string]$Field) {
    $cursor = $Candidate
    while ($true) {
        if (Test-Path -LiteralPath $cursor) {
            try {
                $item = Get-Item -LiteralPath $cursor -Force -ErrorAction Stop
            } catch {
                Fail "$Field could not be inspected: $Candidate"
            }
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                Fail "$Field cannot contain a symlink or junction: $Candidate"
            }
        }
        if ($cursor.Equals($RootPath, [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $parent = Split-Path -Path $cursor -Parent
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent.Equals($cursor, [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $cursor = $parent
    }
}

function Resolve-SafeChildPath([string]$RootPath, [string]$RelativePath, [string]$Field) {
    if ([string]::IsNullOrWhiteSpace($RelativePath)) {
        Fail "$Field must be a nonempty relative path"
    }
    if ([IO.Path]::IsPathRooted($RelativePath) -or
        $RelativePath -match '(^|[\/])\.\.([\/]|$)' -or
        $RelativePath.Contains(':') -or
        $RelativePath.IndexOf([char]0) -ge 0) {
        Fail "$Field contains an unsafe path"
    }
    try {
        $candidate = [IO.Path]::GetFullPath((Join-Path -Path $RootPath -ChildPath ($RelativePath -replace '/', '\')))
    } catch {
        Fail "$Field could not be resolved: $RelativePath"
    }
    $prefix = $RootPath.TrimEnd('\') + '\'
    if (-not $candidate.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        Fail "$Field escapes the selected root: $RelativePath"
    }
    Assert-NoReparseChain $RootPath $candidate $Field
    return $candidate
}

function Resolve-InstallRoot([string]$SourceRoot, [string]$RequestedRoot) {
    try {
        $resolved = [IO.Path]::GetFullPath($RequestedRoot)
    } catch {
        Fail 'install root could not be resolved'
    }
    if ([string]::IsNullOrWhiteSpace($resolved) -or
        $resolved.Equals($SourceRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $resolved.StartsWith($SourceRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Fail 'install root cannot be the bundle directory or one of its children'
    }
    $parent = Split-Path -Path $resolved -Parent
    $leaf = Split-Path -Path $resolved -Leaf
    if ([string]::IsNullOrWhiteSpace($parent) -or [string]::IsNullOrWhiteSpace($leaf)) {
        Fail 'install root must name a directory below an existing parent'
    }
    if (-not (Test-Path -LiteralPath $parent)) {
        if ($Action -eq 'Uninstall') {
            Fail 'install root does not exist'
        }
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Assert-NoReparseChain $parent $parent 'install parent'
    return @{ Root = $resolved; Parent = $parent; Leaf = $leaf }
}

function Assert-RuntimeInstall([string]$RootPath, [string]$ManifestName, [string]$ExpectedManifestPath) {
    $manifestPath = Resolve-SafeChildPath $RootPath $ManifestName 'installed runtime manifest path'
    $verifierPath = Resolve-SafeChildPath $RootPath 'verify-offline-bundle.ps1' 'installed verifier path'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $verifierPath -PathType Leaf)) {
        Fail 'install root is not a Vertex offline runtime'
    }
    try {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        Fail 'installed runtime manifest could not be read'
    }
    if ($manifest.manifest_kind -ne 'runtime' -or $manifest.audit_status -ne 'incomplete') {
        Fail 'installed runtime manifest is unsupported'
    }
    # A marker alone is not proof of ownership. Bind it to the verified source
    # bundle before using its entries to authorize replacement or removal.
    if ((Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $ExpectedManifestPath -Algorithm SHA256).Hash) {
        Fail 'installed runtime manifest does not match this bundle; use the original bundle'
    }
    $ownedFiles = @{}
    $ownedDirectories = @{}
    foreach ($relative in @($ManifestName, 'verify-offline-bundle.ps1') + @($manifest.files | ForEach-Object { $_.path })) {
        $ownedPath = Resolve-SafeChildPath $RootPath $relative 'owned runtime path'
        $ownedFiles[$ownedPath] = $true
        $parent = Split-Path -Path $ownedPath -Parent
        while (-not $parent.Equals($RootPath, [StringComparison]::OrdinalIgnoreCase)) {
            $ownedDirectories[$parent] = $true
            $parent = Split-Path -Path $parent -Parent
        }
    }
    foreach ($candidate in @(Get-ChildItem -LiteralPath $RootPath -Recurse -Force -ErrorAction Stop)) {
        if (($candidate.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail "installed runtime contains a symlink or junction: $($candidate.FullName)"
        }
        if (($candidate.PSIsContainer -and -not $ownedDirectories.ContainsKey($candidate.FullName)) -or
            (-not $candidate.PSIsContainer -and -not $ownedFiles.ContainsKey($candidate.FullName))) {
            Fail "installed runtime contains unowned content; move it outside the install root before $Action`: $($candidate.FullName)"
        }
    }
    if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
        Initialize-ModuleSecurity
        $modulePaths = Get-OwnedModulePaths $RootPath $manifest
        foreach ($path in $modulePaths.Keys) {
            if (-not $modulePaths[$path]) { [VertexOfflineRuntimeSecurity]::AssertSingleLink($path) }
        }
    }
    return @{ ManifestPath = $manifestPath; VerifierPath = $verifierPath; Manifest = $manifest }
}

function Get-OwnedModulePaths([string]$RootPath, $Manifest) {
    $paths = @{}
    foreach ($entry in @($Manifest.files)) {
        $relative = ([string]$entry.path -replace '\\', '/')
        # The broker loads modules from bin and the sibling Qt plugin tree.
        if ($relative -notmatch '^(bin|plugins)/') { continue }
        $path = Resolve-SafeChildPath $RootPath $relative 'owned module path'
        if (Test-Path -LiteralPath $path -PathType Leaf) { $paths[$path] = $false }
        $parent = Split-Path -Path $path -Parent
        while (-not $parent.Equals($RootPath, [StringComparison]::OrdinalIgnoreCase)) {
            Assert-NoReparseChain $RootPath $parent 'owned module directory'
            if (Test-Path -LiteralPath $parent -PathType Container) { $paths[$parent] = $true }
            $parent = Split-Path -Path $parent -Parent
        }
    }
    return $paths
}

function Initialize-ModuleSecurity {
    if ($null -eq ('VertexOfflineRuntimeSecurity' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Principal;
public static class VertexOfflineRuntimeSecurity {
    [StructLayout(LayoutKind.Sequential)]
    struct FileInformation {
        public uint attributes, creationLow, creationHigh, accessLow, accessHigh, writeLow, writeHigh,
            volumeSerial, sizeHigh, sizeLow, links, indexHigh, indexLow;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFileW(string path, uint access, uint share, IntPtr attributes,
        uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileInformationByHandle(IntPtr handle, out FileInformation information);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool GetSecurityDescriptorDacl(IntPtr descriptor, out bool present,
        out IntPtr dacl, out bool defaulted);
    [DllImport("advapi32.dll")]
    static extern uint SetSecurityInfo(IntPtr handle, uint objectType, uint information,
        IntPtr owner, IntPtr group, IntPtr dacl, IntPtr sacl);
    [DllImport("advapi32.dll")]
    static extern uint GetSecurityInfo(IntPtr handle, uint objectType, uint information,
        out IntPtr owner, out IntPtr group, out IntPtr dacl, out IntPtr sacl, out IntPtr descriptor);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool GetAce(IntPtr acl, uint index, out IntPtr ace);
    [DllImport("kernel32.dll")]
    static extern IntPtr LocalFree(IntPtr pointer);
    [DllImport("userenv.dll", CharSet = CharSet.Unicode)]
    static extern int CreateAppContainerProfile(string name, string displayName, string description,
        IntPtr capabilities, uint count, out IntPtr sid);
    [DllImport("userenv.dll", CharSet = CharSet.Unicode)]
    static extern int DeriveAppContainerSidFromAppContainerName(string name, out IntPtr sid);
    [DllImport("advapi32.dll")]
    static extern IntPtr FreeSid(IntPtr sid);
    static IntPtr OpenOwned(string path, uint access) {
        IntPtr handle = CreateFileW(path, access, 7, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero);
        if (handle == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
        return handle;
    }
    static void ValidateObject(IntPtr handle, bool directory) {
        FileInformation info;
        if (!GetFileInformationByHandle(handle, out info)) throw new Win32Exception(Marshal.GetLastWin32Error());
        if ((info.attributes & 0x400) != 0 || ((info.attributes & 0x10) != 0) != directory)
            throw new InvalidOperationException("module path is not an ordinary owned object");
        if (!directory && info.links != 1)
            throw new InvalidOperationException("module hard link is not exclusively owned by this runtime");
    }
    public static void AssertSingleLink(string path) {
        IntPtr handle = OpenOwned(path, 0x80);
        try { ValidateObject(handle, false); } finally { CloseHandle(handle); }
    }
    public static bool HasInheritedDaclAce(string path) {
        IntPtr handle = OpenOwned(path, 0x20000), descriptor = IntPtr.Zero;
        try {
            IntPtr owner, group, dacl, sacl;
            uint result = GetSecurityInfo(handle, 1, 4, out owner, out group, out dacl, out sacl, out descriptor);
            if (result != 0) throw new Win32Exception((int)result);
            if (dacl == IntPtr.Zero) return false;
            int count = (ushort)Marshal.ReadInt16(dacl, 4);
            for (uint i = 0; i < count; ++i) {
                IntPtr ace;
                if (!GetAce(dacl, i, out ace)) throw new Win32Exception(Marshal.GetLastWin32Error());
                if ((Marshal.ReadByte(ace, 1) & 0x10) != 0) return true;
            }
            return false;
        } finally { if (descriptor != IntPtr.Zero) LocalFree(descriptor); CloseHandle(handle); }
    }
    public static void ApplyDacl(string path, byte[] descriptor, bool protect, bool directory) {
        // MAXIMUM_ALLOWED suppresses inherited access-mask propagation. The
        // caller guards unknown children because inherited ACE flags can
        // still change when the parent DACL changes.
        IntPtr handle = OpenOwned(path, 0x02000000);
        GCHandle pinned = GCHandle.Alloc(descriptor, GCHandleType.Pinned);
        try {
            ValidateObject(handle, directory);
            bool present, defaulted;
            IntPtr dacl;
            if (!GetSecurityDescriptorDacl(pinned.AddrOfPinnedObject(), out present, out dacl, out defaulted))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            if (!present || dacl == IntPtr.Zero) throw new InvalidOperationException("module DACL must not be null");
            uint result = SetSecurityInfo(handle, 1, 4 | (protect ? 0x80000000u : 0x20000000u),
                IntPtr.Zero, IntPtr.Zero, dacl, IntPtr.Zero);
            if (result != 0) throw new Win32Exception((int)result);
        } finally { pinned.Free(); CloseHandle(handle); }
    }
    public static string WorkerSid() {
        IntPtr sid = IntPtr.Zero;
        try {
            int result = CreateAppContainerProfile("Vertex.ImportWorker", "Vertex import worker",
                "Local worker for bounded drawing-file interchange", IntPtr.Zero, 0, out sid);
            if (result == unchecked((int)0x800700B7)) {
                if (sid != IntPtr.Zero) { FreeSid(sid); sid = IntPtr.Zero; }
                result = DeriveAppContainerSidFromAppContainerName("Vertex.ImportWorker", out sid);
            }
            if (result < 0 || sid == IntPtr.Zero)
                throw new InvalidOperationException("worker AppContainer profile unavailable: 0x" + result.ToString("X8"));
            return new SecurityIdentifier(sid).Value;
        } finally { if (sid != IntPtr.Zero) FreeSid(sid); }
    }
}
'@
    }
}

function Get-WorkerAppContainerSid {
    Initialize-ModuleSecurity
    return [Security.Principal.SecurityIdentifier]::new([VertexOfflineRuntimeSecurity]::WorkerSid())
}

function Set-ModuleAccessAcl([string]$Path, $Acl, [bool]$IsDirectory) {
    Initialize-ModuleSecurity
    [VertexOfflineRuntimeSecurity]::ApplyDacl($Path, $Acl.GetSecurityDescriptorBinaryForm(), $Acl.AreAccessRulesProtected, $IsDirectory)
}

function Assert-PreservedModuleAcls([string]$RootPath, $Paths) {
    Initialize-ModuleSecurity
    foreach ($module in @('bin', 'plugins')) {
        $moduleRoot = Resolve-SafeChildPath $RootPath $module 'preserved module root'
        if (-not (Test-Path -LiteralPath $moduleRoot)) { continue }
        foreach ($candidate in @(Get-ChildItem -LiteralPath $moduleRoot -Recurse -Force -ErrorAction Stop)) {
            if ($Paths.ContainsKey($candidate.FullName)) { continue }
            if (($candidate.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
                [VertexOfflineRuntimeSecurity]::HasInheritedDaclAce($candidate.FullName)) {
                Fail "cannot safely change module permissions around unowned inherited or reparse content; preserved at $($candidate.FullName)"
            }
        }
    }
}

function Protect-ModuleParentDeletion([string]$RootPath) {
    $existing = Get-Acl -LiteralPath $RootPath
    $acl = [Security.AccessControl.DirectorySecurity]::new()
    $acl.SetSecurityDescriptorSddlForm($existing.Sddl, [Security.AccessControl.AccessControlSections]::Access)
    # DELETE on a child can be granted by its parent even when the child has
    # no DELETE allow ACE. Deny that alternate route on the owned install root
    # only, without inheriting this restriction or touching outside parents.
    $userSid = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        $userSid, 'DeleteSubdirectoriesAndFiles', 'Deny'))
    Set-ModuleAccessAcl $RootPath $acl $true
}

function Protect-OwnedModuleTree([string]$RootPath, $Manifest, [bool]$PreserveUnowned = $false) {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { return }
    $paths = Get-OwnedModulePaths $RootPath $Manifest
    if ($PreserveUnowned) {
        Assert-PreservedModuleAcls $RootPath $paths
    } else {
        foreach ($module in @('bin', 'plugins')) {
            $moduleRoot = Resolve-SafeChildPath $RootPath $module 'module root'
            if (-not (Test-Path -LiteralPath $moduleRoot)) { continue }
            foreach ($candidate in @(Get-ChildItem -LiteralPath $moduleRoot -Recurse -Force -ErrorAction Stop)) {
                if (-not $paths.ContainsKey($candidate.FullName) -or
                    ($candidate.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                    Fail "module tree contains unowned content before permission changes: $($candidate.FullName)"
                }
            }
        }
    }
    $userSid = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $workerSid = Get-WorkerAppContainerSid
    foreach ($path in $paths.Keys) {
        if (-not $paths[$path]) { [VertexOfflineRuntimeSecurity]::AssertSingleLink($path) }
    }
    $systemSid = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
    $adminSid = [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
    # Explicit ACLs on every declared object avoid relying on inherited grants
    # or propagating changes to content not owned by this bundle.
    foreach ($path in @($paths.Keys | Sort-Object { $_.Length } -Descending)) {
        $acl = if ($paths[$path]) { [Security.AccessControl.DirectorySecurity]::new() } `
               else { [Security.AccessControl.FileSecurity]::new() }
        $acl.SetAccessRuleProtection($true, $false)
        foreach ($sid in @($systemSid, $adminSid)) {
            $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid, 'FullControl', 'Allow'))
        }
        foreach ($sid in @($userSid, $workerSid)) {
            $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid, 'ReadAndExecute', 'Allow'))
        }
        if ($paths[$path] -and -not $PreserveUnowned) {
            # Recheck immediately before each directory update. Windows may
            # rewrite inherited ACE flags on unexpected children even when
            # the native API does not propagate access masks to them.
            foreach ($child in @(Get-ChildItem -LiteralPath $path -Force -ErrorAction Stop)) {
                if (-not $paths.ContainsKey($child.FullName) -or
                    ($child.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                    Fail "module directory received unowned content before permission changes: $($child.FullName)"
                }
            }
        }
        Set-ModuleAccessAcl $path $acl $paths[$path]
    }
    Protect-ModuleParentDeletion $RootPath
}

function Enable-OwnedModuleRemoval([string]$RootPath, $Manifest) {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { return }
    $paths = Get-OwnedModulePaths $RootPath $Manifest
    Initialize-ModuleSecurity
    Assert-PreservedModuleAcls $RootPath $paths
    foreach ($path in $paths.Keys) {
        if (-not $paths[$path]) { [VertexOfflineRuntimeSecurity]::AssertSingleLink($path) }
    }
    $userSid = [Security.Principal.WindowsIdentity]::GetCurrent().User
    foreach ($path in @($paths.Keys | Sort-Object { $_.Length } -Descending)) {
        $existing = Get-Acl -LiteralPath $path
        $acl = if ($paths[$path]) { [Security.AccessControl.DirectorySecurity]::new() } `
               else { [Security.AccessControl.FileSecurity]::new() }
        $acl.SetSecurityDescriptorSddlForm($existing.Sddl, [Security.AccessControl.AccessControlSections]::Access)
        # No inheritance: grant deletion only on exact manifest-owned objects.
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($userSid, 'Modify', 'Allow'))
        Set-ModuleAccessAcl $path $acl $paths[$path]
    }
}

function Remove-OwnedRuntime([string]$RootPath, [string]$ManifestName, $Manifest) {
    # Delete only declared runtime files. Nonrecursive directory removal leaves
    # any content that appeared after validation intact and makes the caller
    # handle the incomplete cleanup explicitly.
    $files = @($Manifest.files | ForEach-Object {
        Resolve-SafeChildPath $RootPath ([string]$_.path) 'owned runtime removal path'
    }) + @(
        (Resolve-SafeChildPath $RootPath $ManifestName 'owned runtime manifest removal path'),
        (Resolve-SafeChildPath $RootPath 'verify-offline-bundle.ps1' 'owned verifier removal path')
    )
    $directories = @{}
    foreach ($file in $files) {
        $parent = Split-Path -Path $file -Parent
        while (-not $parent.Equals($RootPath, [StringComparison]::OrdinalIgnoreCase)) {
            $directories[$parent] = $true
            $parent = Split-Path -Path $parent -Parent
        }
    }
    Enable-OwnedModuleRemoval $RootPath $Manifest
    $complete = $true
    foreach ($file in $files) {
        if (Test-Path -LiteralPath $file -PathType Leaf) {
            try { [IO.File]::Delete($file) } catch { $complete = $false }
        }
    }
    foreach ($directory in @($directories.Keys | Sort-Object { $_.Length } -Descending)) {
        if (Test-Path -LiteralPath $directory -PathType Container) {
            try { [IO.Directory]::Delete($directory, $false) } catch { $complete = $false }
        }
    }
    if (Test-Path -LiteralPath $RootPath -PathType Container) {
        try { [IO.Directory]::Delete($RootPath, $false) } catch { $complete = $false }
    }
    return $complete -and -not (Test-Path -LiteralPath $RootPath)
}

function Restore-OwnedRuntime([string]$SourceRoot, [string]$DestinationRoot,
                               [string]$ManifestName, [string]$ManifestPath,
                               [string]$VerifierPath, $Manifest) {
    New-Item -ItemType Directory -Path $DestinationRoot -Force | Out-Null
    foreach ($entry in @($Manifest.files)) {
        $sourcePath = Resolve-SafeChildPath $SourceRoot ([string]$entry.path) 'runtime restore source path'
        $destinationPath = Resolve-SafeChildPath $DestinationRoot ([string]$entry.path) 'runtime restore destination path'
        New-Item -ItemType Directory -Path (Split-Path -Parent $destinationPath) -Force | Out-Null
        Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
    }
    Copy-Item -LiteralPath $ManifestPath -Destination (Join-Path $DestinationRoot $ManifestName) -Force
    Copy-Item -LiteralPath $VerifierPath -Destination (Join-Path $DestinationRoot 'verify-offline-bundle.ps1') -Force
}

try {
    $sourceRoot = [IO.Path]::GetFullPath($PSScriptRoot)
    $sourceVerifier = Join-Path $sourceRoot 'verify-offline-bundle.ps1'
    if (-not (Test-Path -LiteralPath $sourceVerifier -PathType Leaf)) {
        Fail 'the bundle verifier is missing'
    }
    & $sourceVerifier -Root $sourceRoot -ManifestName 'offline-bundle-manifest.json'
    if ($LASTEXITCODE -ne 0) {
        Fail 'bundle verification failed; no files were installed'
    }

    $bundleManifestPath = Join-Path $sourceRoot 'offline-bundle-manifest.json'
    $bundleManifest = Get-Content -LiteralPath $bundleManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $runtimeManifestName = [string]$bundleManifest.installer.runtime_manifest
    if ([string]::IsNullOrWhiteSpace($runtimeManifestName)) {
        $runtimeManifestName = 'runtime-manifest.json'
    }
    $runtimeManifestPath = Resolve-SafeChildPath $sourceRoot $runtimeManifestName 'runtime manifest path'
    $runtimeManifest = Get-Content -LiteralPath $runtimeManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($runtimeManifest.manifest_kind -ne 'runtime' -or $runtimeManifest.audit_status -ne 'incomplete') {
        Fail 'runtime manifest is unsupported'
    }

    $target = Resolve-InstallRoot $sourceRoot $InstallRoot
    $targetRoot = $target.Root
    $targetParent = $target.Parent
    $targetLeaf = $target.Leaf

    $targetInitiallyExists = Test-Path -LiteralPath $targetRoot
    if ($targetInitiallyExists) {
        $targetItem = Get-Item -LiteralPath $targetRoot -Force
        if (-not $targetItem.PSIsContainer) {
            Fail 'install root is not a directory'
        }
        if (($targetItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail 'install root cannot be a symlink or junction'
        }
        if ($Action -eq 'Install' -and @(Get-ChildItem -LiteralPath $targetRoot -Force).Count -ne 0) {
            Fail 'install root must be missing or empty'
        }
    }

    if ($Action -eq 'Uninstall') {
        if (-not $targetInitiallyExists) {
            Fail 'install root does not exist'
        }
        [void](Assert-RuntimeInstall $targetRoot $runtimeManifestName $runtimeManifestPath)
        # Never execute a script from the tree being checked for tampering.
        if ((Get-FileHash -LiteralPath (Join-Path $targetRoot 'verify-offline-bundle.ps1') -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $sourceVerifier -Algorithm SHA256).Hash) {
            Fail 'installed verifier does not match this bundle; repair before uninstalling'
        }
        & $sourceVerifier -Root $targetRoot -ManifestName $runtimeManifestName
        if ($LASTEXITCODE -ne 0) {
            Fail 'installed runtime verification failed; refusing to remove it'
        }
        $installed = Assert-RuntimeInstall $targetRoot $runtimeManifestName $runtimeManifestPath
        if (-not (Remove-OwnedRuntime $targetRoot $runtimeManifestName $installed.Manifest)) {
            Fail 'uninstall preserved content that appeared after validation; the install root was not removed'
        }
        Write-Output ("Removed the verified Vertex runtime from {0}." -f $targetRoot)
        exit 0
    }

    if ($Action -eq 'Repair') {
        if (-not $targetInitiallyExists) {
            Fail 'repair target does not exist'
        }
        # A repair may replace damaged payload bytes, but it must still prove
        # that the destination belongs to this exact bundle
        # before moving it aside.
        [void](Assert-RuntimeInstall $targetRoot $runtimeManifestName $runtimeManifestPath)
    }

    $installToken = [Guid]::NewGuid().ToString('N')
    $stagingRoot = Join-Path $targetParent ('.{0}.installing-{1}' -f $targetLeaf, $installToken)
    $backupRoot = Join-Path $targetParent ('.{0}.backup-{1}' -f $targetLeaf, $installToken)
    if ((Test-Path -LiteralPath $stagingRoot) -or (Test-Path -LiteralPath $backupRoot)) {
        Fail 'temporary install paths already exist'
    }
    $targetMovedToBackup = $false
    $backupRecoveryFailed = $false
    $publishedRootCreated = $false
    $published = $false
    try {
        New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null
        Assert-NoReparseChain $targetParent $stagingRoot 'staging root'

        foreach ($entry in @($runtimeManifest.files)) {
            $relative = ($entry.path -replace '\\', '/')
            $sourcePath = Resolve-SafeChildPath $sourceRoot $relative 'runtime source path'
            $stagingPath = Resolve-SafeChildPath $stagingRoot $relative 'runtime staging path'
            if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
                Fail "runtime source file is missing: $relative"
            }
            New-Item -ItemType Directory -Path (Split-Path -Parent $stagingPath) -Force | Out-Null
            Copy-Item -LiteralPath $sourcePath -Destination $stagingPath -Force
        }
        Copy-Item -LiteralPath $runtimeManifestPath -Destination (Join-Path $stagingRoot $runtimeManifestName) -Force
        Copy-Item -LiteralPath $sourceVerifier -Destination (Join-Path $stagingRoot 'verify-offline-bundle.ps1') -Force

        & $sourceVerifier -Root $stagingRoot -ManifestName $runtimeManifestName
        if ($LASTEXITCODE -ne 0) {
            Fail 'staged runtime verification failed; no files were installed'
        }

        # Publish by directory rename after the complete staged tree verifies.
        # If an empty destination existed, retain it beside the staging tree
        # until the new install has passed its post-publish verification.
        if ($targetInitiallyExists) {
            Move-Item -LiteralPath $targetRoot -Destination $backupRoot
            $targetMovedToBackup = $true
            if ($Action -eq 'Repair') {
                # Close the scan-to-rename interval before publishing. A late
                # user file rejects the repair and the catch path restores the
                # untouched backup to its original name.
                [void](Assert-RuntimeInstall $backupRoot $runtimeManifestName $runtimeManifestPath)
            } elseif (@(Get-ChildItem -LiteralPath $backupRoot -Force -ErrorAction Stop).Count -ne 0) {
                Fail 'install destination received content during publication; the original directory will be restored'
            }
        }
        Move-Item -LiteralPath $stagingRoot -Destination $targetRoot
        $publishedRootCreated = $true

        & $sourceVerifier -Root $targetRoot -ManifestName $runtimeManifestName
        if ($LASTEXITCODE -ne 0) {
            Fail 'installed runtime verification failed'
        }
        # Verify ownership after publication before changing permissions. A
        # failed ACL update follows the same owned-file rollback as any other
        # publication failure, including a partially frozen tree.
        $publishedInstall = Assert-RuntimeInstall $targetRoot $runtimeManifestName $runtimeManifestPath
        Protect-OwnedModuleTree $targetRoot $publishedInstall.Manifest
        # Close the ACL-update interval before accepting the publication. A
        # late unknown object must not become a trusted worker module merely
        # because the parent directory has now been made read-only.
        [void](Assert-RuntimeInstall $targetRoot $runtimeManifestName $runtimeManifestPath)
        & $sourceVerifier -Root $targetRoot -ManifestName $runtimeManifestName
        if ($LASTEXITCODE -ne 0) {
            Fail 'protected runtime verification failed'
        }
        if (Test-Path -LiteralPath $backupRoot) {
            if ($Action -eq 'Repair') {
                $backup = Assert-RuntimeInstall $backupRoot $runtimeManifestName $runtimeManifestPath
                $cleanupError = $null
                try {
                    $backupRemoved = Remove-OwnedRuntime $backupRoot $runtimeManifestName $backup.Manifest
                } catch {
                    $cleanupError = $_.Exception.Message
                    $backupRemoved = $false
                }
                if (-not $backupRemoved) {
                    # Reconstitute the old runtime around the preserved late content
                    # so rollback can return a usable installation at the same path.
                    $backupRecoveryFailed = $true
                    Enable-OwnedModuleRemoval $backupRoot $runtimeManifest
                    Restore-OwnedRuntime $sourceRoot $backupRoot $runtimeManifestName `
                            $runtimeManifestPath $sourceVerifier $runtimeManifest
                    Protect-OwnedModuleTree $backupRoot $runtimeManifest $true
                    $backupRecoveryFailed = $false
                    if ($null -ne $cleanupError) {
                        Fail "repair cleanup failed; the original runtime will be restored: $cleanupError"
                    }
                    Fail "repair preserved content that appeared during publication; the original runtime will be restored"
                }
            } else {
                try { [IO.Directory]::Delete($backupRoot, $false) } catch { }
            }
        }
        $published = $true
    } catch {
        $operationError = $_.Exception.Message
        $rollbackMessage = $null
        if (Test-Path -LiteralPath $stagingRoot) {
            Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
        if (-not $published) {
            if ($publishedRootCreated -and (Test-Path -LiteralPath $targetRoot) -and -not $backupRecoveryFailed) {
                $publishedInstall = $null
                try {
                    $publishedInstall = Assert-RuntimeInstall $targetRoot $runtimeManifestName $runtimeManifestPath
                } catch {
                    # A changed publication may contain data created after the
                    # rename. Never recursively erase it during rollback.
                }
                if ($null -ne $publishedInstall) {
                    [void](Remove-OwnedRuntime $targetRoot $runtimeManifestName $publishedInstall.Manifest)
                }
            }
            if ($targetMovedToBackup -and (Test-Path -LiteralPath $backupRoot) -and
                -not (Test-Path -LiteralPath $targetRoot) -and -not $backupRecoveryFailed) {
                Move-Item -LiteralPath $backupRoot -Destination $targetRoot -Force
            } elseif ($backupRecoveryFailed -and (Test-Path -LiteralPath $backupRoot)) {
                $rollbackMessage = "rollback could not recover the original runtime; verified protected publication retained at $targetRoot; backup content retained at $backupRoot"
            } elseif ($targetMovedToBackup -and (Test-Path -LiteralPath $backupRoot) -and
                      (Test-Path -LiteralPath $targetRoot)) {
                $rollbackMessage = ("rollback preserved both changed trees; published content: {0}; original runtime: {1}" -f `
                    $targetRoot, $backupRoot)
            }
        }
        if ($null -ne $rollbackMessage) {
            throw "$operationError; $rollbackMessage"
        }
        throw
    }
    if (Test-Path -LiteralPath $backupRoot) {
        Fail "verified backup cleanup was incomplete; preserved content remains at $backupRoot"
    }
    $verb = if ($Action -eq 'Repair') { 'Repaired' } else { 'Installed' }
    Write-Output ("{0} {1} runtime files to {2}; qualification remains incomplete." -f $verb, @($runtimeManifest.files).Count, $targetRoot)
    exit 0
} catch {
    Write-Error $_.Exception.Message
    exit 1
}
