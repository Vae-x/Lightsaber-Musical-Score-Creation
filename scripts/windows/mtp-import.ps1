# 本脚本只从 Windows Shell 的便携设备命名空间读取文件，不向头显写入任何内容。
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$utf8 = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

function Send-Record {
    param($Record)
    [Console]::WriteLine(($Record | ConvertTo-Json -Depth 12 -Compress))
}

function Find-UniqueItem {
    param($Folder, [string]$Name)
    $matches = @($Folder.Items() | Where-Object { [string]::Equals([string]$_.Name, $Name, [StringComparison]::OrdinalIgnoreCase) })
    if ($matches.Count -ne 1) { throw "设备目录不存在或名称重复：$Name。请重新刷新歌曲列表。" }
    return $matches[0]
}

function Get-SongSources {
    @(
        [pscustomobject]@{ GameName = '星穹绿洲'; Segments = @('SoulTopia', 'BeatNote', 'Custom') }
        [pscustomobject]@{ GameName = '光之乐团'; Segments = @('Android', 'data', 'com.StarRiverVR.LightBand', 'files', 'CustomMusic') }
    )
}

function Test-SupportedLocator {
    param($Locator)
    if ([string]::IsNullOrWhiteSpace([string]$Locator.device)) { return $false }
    $segments = @($Locator.segments)
    foreach ($segment in $segments) {
        if ($segment -isnot [string] -or [string]::IsNullOrWhiteSpace($segment)) { return $false }
    }
    foreach ($source in Get-SongSources) {
        if ($segments.Count -ne $source.Segments.Count + 2) { continue }
        $matches = $true
        for ($index = 0; $index -lt $source.Segments.Count; $index++) {
            if ($segments[$index + 1] -cne $source.Segments[$index]) { $matches = $false; break }
        }
        if ($matches) { return $true }
    }
    return $false
}

function Find-SongRootFolder {
    param($Storage, [string[]]$Segments)
    $folder = $Storage.GetFolder
    foreach ($segment in $Segments) {
        $items = @($folder.Items() | Where-Object { [string]::Equals([string]$_.Name, $segment, [StringComparison]::OrdinalIgnoreCase) })
        if ($items.Count -ne 1 -or -not $items[0].IsFolder) { return $null }
        $folder = $items[0].GetFolder
    }
    return $folder
}

function Assert-SafeName {
    param([string]$Name)
    if ([string]::IsNullOrWhiteSpace($Name) -or $Name -eq '.' -or $Name -eq '..' -or
        $Name.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or
        $Name.EndsWith('.') -or $Name.EndsWith(' ') -or
        $Name -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])($|\.)') {
        throw "头显中存在无法安全复制到 Windows 的文件名：$Name"
    }
}

function Get-Manifest {
    param($Folder, [string]$Prefix, [int]$Depth)
    if ($Depth -gt 20) { throw '歌曲的子目录层级超过限制。' }
    $names = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($item in $Folder.Items()) {
        $name = [string]$item.Name
        Assert-SafeName $name
        if (-not $names.Add($name)) { throw "歌曲中存在同名文件：$name" }
        $relative = if ($Prefix) { $Prefix + '\' + $name } else { $name }
        if ($item.IsFolder) {
            Get-Manifest $item.GetFolder $relative ($Depth + 1)
        } else {
            $size = -1L
            try { $size = [long]$item.ExtendedProperty('System.Size') } catch { }
            if ($size -lt 0) {
                try { $size = [long]$item.Size } catch { }
            }
            if ($size -lt 0) { throw "无法确定文件大小，不能验证复制是否完成：$relative" }
            [pscustomobject]@{ Relative = $relative; Size = $size; Item = $item }
        }
    }
}

function Assert-LocalTarget {
    $session = [IO.Path]::GetFullPath($env:LMSC_MTP_SESSION).TrimEnd('\')
    $target = [IO.Path]::GetFullPath($env:LMSC_MTP_TARGET).TrimEnd('\')
    if ($session.StartsWith('\\') -or $target.StartsWith('\\') -or
        -not [string]::Equals($target, ($session + '\song'), [StringComparison]::OrdinalIgnoreCase) -or
        -not [IO.Directory]::Exists($target)) { throw '导入目标必须是软件创建的本地临时目录。' }
    $marker = Join-Path $session '.lmsc-mtp-session'
    if (-not [IO.File]::Exists($marker) -or [IO.File]::ReadAllText($marker) -ne $env:LMSC_MTP_TOKEN) {
        throw '导入临时目录校验失败。'
    }
    if (@([IO.Directory]::EnumerateFileSystemEntries($target)).Count -ne 0) { throw '导入临时目录不为空，已停止复制。' }
    $attributes = [IO.File]::GetAttributes($target)
    if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw '导入目录不能是重解析点。' }
    return $target
}

function Assert-CompleteSong {
    param([string]$Target)
    $infoFiles = @(Get-ChildItem -LiteralPath $Target -File | Where-Object { $_.Name -ieq 'Info.dat' })
    if ($infoFiles.Count -ne 1) { throw '复制后的歌曲缺少唯一的 Info.dat。' }
    try { $info = [IO.File]::ReadAllText($infoFiles[0].FullName, [Text.Encoding]::UTF8) | ConvertFrom-Json } catch { throw '歌曲 Info.dat 不是有效的 JSON 文件。' }
    $references = New-Object 'System.Collections.Generic.List[string]'
    $audio = [string]$info._songFilename
    if (-not $audio) { $audio = [string]$info.audio.songFilename }
    if (-not $audio) { $audio = [string]$info.songFilename }
    if (-not $audio) { throw 'Info.dat 中没有声音文件名。' }
    [void]$references.Add($audio)
    if ($info._coverImageFilename) { [void]$references.Add([string]$info._coverImageFilename) }
    foreach ($set in $info._difficultyBeatmapSets) {
        foreach ($beatmap in $set._difficultyBeatmaps) {
            if ($beatmap._beatmapFilename) { [void]$references.Add([string]$beatmap._beatmapFilename) }
        }
    }
    foreach ($beatmap in $info.difficultyBeatmaps) {
        if ($beatmap.beatmapDataFilename) { [void]$references.Add([string]$beatmap.beatmapDataFilename) }
        if ($beatmap.lightshowDataFilename) { [void]$references.Add([string]$beatmap.lightshowDataFilename) }
    }
    foreach ($reference in $references) {
        if ([IO.Path]::IsPathRooted($reference)) { throw 'Info.dat 引用绝对路径，不能安全导入。' }
        $parts = @($reference -split '[\\/]')
        foreach ($part in $parts) { Assert-SafeName $part }
        $path = [IO.Path]::GetFullPath((Join-Path $Target ($parts -join '\')))
        if (-not $path.StartsWith(($Target + '\'), [StringComparison]::OrdinalIgnoreCase)) { throw 'Info.dat 引用了歌曲目录之外的文件。' }
        if (-not [IO.File]::Exists($path) -and $reference -eq $audio -and [IO.Path]::GetExtension($path) -ieq '.egg') {
            # 星穹绿洲设备上的声音文件有时为 .ogg，而原始 Info.dat 仍引用 .egg。
            $path = [IO.Path]::ChangeExtension($path, '.ogg')
        }
        if (-not [IO.File]::Exists($path) -or (Get-Item -LiteralPath $path).Length -le 0) { throw "复制后的歌曲缺少被引用的文件：$reference" }
    }
}

try {
    $shell = New-Object -ComObject Shell.Application
    $computer = $shell.NameSpace(17)
    if ($null -eq $computer) { throw 'Windows 无法访问“此电脑”。' }
    if ($env:LMSC_MTP_MODE -eq 'list') {
        $songs = New-Object 'System.Collections.Generic.List[object]'
        $devices = @($computer.Items() | Where-Object { $_.IsFolder -and $_.Name -match '(?i)pico' })
        if ($devices.Count -eq 0) { throw '未找到 Pico 头显。请连接 USB，解锁头显，并允许文件传输；设备应出现在 Windows 的“此电脑”中。' }
        foreach ($device in $devices) {
            Send-Record @{ type = 'progress'; message = ('读取 ' + [string]$device.Name + ' 的歌曲目录'); percent = -1 }
            foreach ($storage in $device.GetFolder.Items()) {
                if (-not $storage.IsFolder) { continue }
                foreach ($source in Get-SongSources) {
                    $custom = Find-SongRootFolder $storage $source.Segments
                    if ($null -eq $custom) { continue }
                    foreach ($song in $custom.Items()) {
                        if (-not $song.IsFolder) { continue }
                        if ($songs.Count -ge 4000) { throw '头显歌曲数量超过本次读取限制（4000 首）。' }
                        $root = @([string]$storage.Name) + $source.Segments
                        $segments = $root + @([string]$song.Name)
                        [void]$songs.Add(@{ name = [string]$song.Name; deviceName = [string]$device.Name; gameName = $source.GameName; location = ($root -join '\'); locator = @{ device = [string]$device.Name; segments = $segments } })
                    }
                }
            }
        }
        if ($songs.Count -eq 0) { throw '头显中未找到歌曲文件夹。星穹绿洲：SoulTopia\BeatNote\Custom；光之乐团：Android\data\com.StarRiverVR.LightBand\files\CustomMusic。请确认已导入歌曲，并允许读取内部共享存储空间。' }
        Send-Record @{ type = 'songs'; songs = @($songs.ToArray()) }
        exit 0
    }
    if ($env:LMSC_MTP_MODE -ne 'import') { throw '设备导入操作无效。' }
    $target = Assert-LocalTarget
    $locator = $env:LMSC_MTP_LOCATOR | ConvertFrom-Json
    if (-not (Test-SupportedLocator $locator)) { throw '设备定位信息无效，请刷新歌曲列表。' }
    $device = Find-UniqueItem $computer ([string]$locator.device)
    if (-not $device.IsFolder -or $device.Name -notmatch '(?i)pico') { throw '定位的设备不是 Pico 头显。' }
    $folder = $device.GetFolder
    foreach ($segment in $locator.segments) {
        $item = Find-UniqueItem $folder ([string]$segment)
        if (-not $item.IsFolder) { throw "设备目录已变化：$segment" }
        $folder = $item.GetFolder
    }
    Send-Record @{ type = 'progress'; message = '核对头显歌曲文件'; percent = -1 }
    $manifest = @(Get-Manifest $folder '' 0)
    if ($manifest.Count -eq 0 -or $manifest.Count -gt 10000) { throw '歌曲文件数量为空或超过限制（10000 个）。' }
    $totalBytes = [long](($manifest | Measure-Object -Property Size -Sum).Sum)
    if ($totalBytes -gt 8GB) { throw '单首歌曲超过 8 GB，已停止导入。' }
    $completedBytes = 0L
    $completedFiles = 0
    $timer = [Diagnostics.Stopwatch]::StartNew()
    foreach ($file in $manifest) {
        if ($timer.Elapsed.TotalMinutes -gt 14) { throw '读取歌曲超时，请重新连接头显。' }
        $local = [IO.Path]::GetFullPath((Join-Path $target $file.Relative))
        if (-not $local.StartsWith(($target + '\'), [StringComparison]::OrdinalIgnoreCase)) { throw '歌曲文件越过导入目录，已停止读取。' }
        $parent = [IO.Path]::GetDirectoryName($local)
        [void][IO.Directory]::CreateDirectory($parent)
        $destination = $shell.NameSpace($parent)
        if ($null -eq $destination) { throw 'Windows 无法打开本地导入目录。' }
        # CopyHere 的接收方只可能是以上校验后的本地目录，源始终是 MTP FolderItem。
        # Microsoft 文档说明没有完成通知，因此逐个核对大小、稳定时间和可独占读取状态。
        $destination.CopyHere($file.Item, 1556)
        $fileTimer = [Diagnostics.Stopwatch]::StartNew()
        $stable = 0
        do {
            Start-Sleep -Milliseconds 250
            $verified = $false
            if ([IO.File]::Exists($local)) {
                try {
                    $stream = [IO.File]::Open($local, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
                    try { $verified = ($stream.Length -eq $file.Size) } finally { $stream.Dispose() }
                } catch { }
            }
            if ($verified) { $stable++ } else { $stable = 0 }
            if ($fileTimer.Elapsed.TotalSeconds -gt 180) { throw "复制文件超时或大小不一致：$($file.Relative)" }
        } while ($stable -lt 4)
        $completedBytes += $file.Size
        $completedFiles++
        $percent = if ($totalBytes -gt 0) { [int][Math]::Min(99, 100.0 * $completedBytes / $totalBytes) } else { [int][Math]::Min(99, 100.0 * $completedFiles / $manifest.Count) }
        Send-Record @{ type = 'progress'; message = ('复制头显歌曲：' + $completedFiles + '/' + $manifest.Count); percent = $percent }
    }
    $copiedFiles = @(Get-ChildItem -LiteralPath $target -File -Recurse)
    $copiedBytes = [long](($copiedFiles | Measure-Object -Property Length -Sum).Sum)
    if ($copiedFiles.Count -ne $manifest.Count -or $copiedBytes -ne $totalBytes) { throw '复制后的文件数量或总字节数不匹配，未完成导入。' }
    Assert-CompleteSong $target
    Send-Record @{ type = 'imported'; path = $target; files = $manifest.Count; bytes = $totalBytes }
    exit 0
} catch {
    Send-Record @{ type = 'error'; message = $_.Exception.Message }
    exit 1
}
