# 同步自写模块、头文件和相对路径，不改动用户的调试器与下载配置。
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$mdk = Join-Path $root 'MDK-ARM'
$project = Join-Path $mdk 'XIAOLI-CODE.uvprojx'
$xml = New-Object System.Xml.XmlDocument
$xml.PreserveWhitespace = $false
$xml.Load($project)
foreach ($target in $xml.SelectNodes('/Project/Targets/Target')) {
    $groups = $target.SelectSingleNode('Groups')
    $seen = @{}
    foreach ($file in @($groups.SelectNodes('Group/Files/File'))) {
        $node = $file.SelectSingleNode('FilePath')
        # 删除生成器的空占位项；有名称的 RTE 托管文件仍然保留。
        if ($null -eq $node) {
            if ($null -eq $file.SelectSingleNode('FileName')) { [void]$file.ParentNode.RemoveChild($file) }
            continue
        }
        $path = $node.InnerText.Replace('\', '/')
        # CubeMX 6.3 命令行可能产生 ../D:/...，规范为项目内相对路径。
        if ($path.StartsWith('../') -and $path.Substring(3) -match '^[A-Za-z]:/') {
            $path = $path.Substring(3)
        }
        if ([IO.Path]::IsPathRooted($path)) {
            $absolute = [IO.Path]::GetFullPath($path)
        } else {
            $absolute = [IO.Path]::GetFullPath((Join-Path $mdk $path))
        }
        if (!(Test-Path -LiteralPath $absolute -PathType Leaf)) {
            throw "Missing project source: $absolute"
        }
        if ($seen.ContainsKey($absolute)) {
            [void]$file.ParentNode.RemoveChild($file)
            continue
        }
        $seen[$absolute] = $true
        $uri = [Uri]($mdk.TrimEnd('\') + '\')
        $node.InnerText = [Uri]::UnescapeDataString($uri.MakeRelativeUri([Uri]$absolute).ToString())
    }
    foreach ($group in @($groups.SelectNodes('Group'))) {
        if ($group.GroupName -like 'Application/User/*' -and $group.SelectNodes('Files/File').Count -eq 0) {
            [void]$groups.RemoveChild($group)
        }
    }
    $name = 'Application/User/Application'
    $group = $groups.SelectSingleNode("Group[GroupName='$name']")
    if ($null -eq $group) {
        $group = $xml.CreateElement('Group')
        $label = $xml.CreateElement('GroupName'); $label.InnerText = $name
        [void]$group.AppendChild($label)
        [void]$group.AppendChild($xml.CreateElement('Files'))
        [void]$groups.AppendChild($group)
    }
    $files = $group.SelectSingleNode('Files')
    foreach ($item in Get-ChildItem -LiteralPath (Join-Path $root 'Application/Src') -Filter '*.c') {
        if ($seen.ContainsKey($item.FullName)) { continue }
        $file = $xml.CreateElement('File')
        foreach ($pair in @(@('FileName', $item.Name), @('FileType', '1'), @('FilePath', "../Application/Src/$($item.Name)"))) {
            $node = $xml.CreateElement($pair[0]); $node.InnerText = $pair[1]
            [void]$file.AppendChild($node)
        }
        [void]$files.AppendChild($file)
    }
    $headers = $groups.SelectSingleNode("Group[GroupName='Application/Headers']")
    if ($null -eq $headers) {
        $headers = $xml.CreateElement('Group')
        $node = $xml.CreateElement('GroupName'); $node.InnerText = 'Application/Headers'
        [void]$headers.AppendChild($node)
        [void]$headers.AppendChild($xml.CreateElement('Files'))
        [void]$groups.AppendChild($headers)
    }
    foreach ($item in Get-ChildItem -LiteralPath (Join-Path $root 'Application/Inc') -Filter '*.h') {
        if ($headers.SelectNodes("Files/File[FileName='$($item.Name)']").Count -ne 0) { continue }
        $file = $xml.CreateElement('File')
        foreach ($pair in @(@('FileName', $item.Name), @('FileType', '5'), @('FilePath', "../Application/Inc/$($item.Name)"))) {
            $node = $xml.CreateElement($pair[0]); $node.InnerText = $pair[1]
            [void]$file.AppendChild($node)
        }
        [void]$headers.SelectSingleNode('Files').AppendChild($file)
    }
    $includes = $target.SelectSingleNode('TargetOption/TargetArmAds/Cads/VariousControls/IncludePath')
    $paths = @($includes.InnerText.Split(';') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    if ($paths -notcontains '../Application/Inc') { $paths += '../Application/Inc' }
    if ($paths -notcontains '../ThirdParty/jsmn') { $paths += '../ThirdParty/jsmn' }
    $includes.InnerText = ($paths | Select-Object -Unique) -join ';'
    # F407ZG 的最后 128 KiB 扇区仅供配置日志，生成工程后也强制恢复此边界。
    foreach ($memory in $target.SelectNodes('TargetOption/TargetArmAds/ArmAdsMisc/OnChipMemories/IROM | TargetOption/TargetArmAds/ArmAdsMisc/OnChipMemories/OCR_RVCT4')) {
        if ($memory.StartAddress -eq '0x8000000') { $memory.Size = '0xE0000' }
    }
    $cpu = $target.SelectSingleNode('TargetOption/TargetCommonOption/Cpu')
    $cpu.InnerText = $cpu.InnerText.Replace('IROM(0x8000000-0x80FFFFF)', 'IROM(0x8000000-0x80DFFFF)')
    $vendor = $headers.SelectSingleNode("Files/File[FileName='jsmn.h']")
    if ($null -eq $vendor) {
        $file = $xml.CreateElement('File')
        foreach ($pair in @(@('FileName', 'jsmn.h'), @('FileType', '5'), @('FilePath', '../ThirdParty/jsmn/jsmn.h'))) {
            $node = $xml.CreateElement($pair[0]); $node.InnerText = $pair[1]
            [void]$file.AppendChild($node)
        }
        [void]$headers.SelectSingleNode('Files').AppendChild($file)
    }
}
$settings = New-Object System.Xml.XmlWriterSettings
$settings.Indent = $true
$settings.Encoding = New-Object System.Text.UTF8Encoding($false)
$writer = [System.Xml.XmlWriter]::Create($project, $settings)
try { $xml.Save($writer) } finally { $writer.Dispose() }
Write-Output 'Keil project synchronized.'
