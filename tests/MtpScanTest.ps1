[CmdletBinding()]
param(
    [string]$SourcePath = (Join-Path $PSScriptRoot '../scripts/windows/mtp-import.ps1')
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
$SourcePath = [IO.Path]::GetFullPath($SourcePath)
$scanTokens = $null
$scanErrors = $null
$scanAst = [Management.Automation.Language.Parser]::ParseFile($SourcePath, [ref]$scanTokens, [ref]$scanErrors)
if ($scanErrors.Count -ne 0) { throw '真实 MTP 导入脚本存在语法错误。' }

# 只加载真实扫描函数的 AST，不 dot-source 脚本，也不执行末尾设备主流程。
$functionNames = @('Find-UniqueItem', 'Get-SongSources', 'Test-SupportedLocator',
    'Find-SongRootFolder', 'Assert-SafeName', 'Find-CategorizedSongs')
foreach ($functionName in $functionNames) {
    $definitions = @($scanAst.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $functionName
    }, $true))
    if ($definitions.Count -ne 1) { throw ('扫描函数未能唯一定位：' + $functionName) }
    Invoke-Expression $definitions[0].Extent.Text
}

# COM 相似对象只存在于内存：Items()/GetFolder/Name/IsFolder 与 Shell 扫描所用契约一致。
Add-Type -TypeDefinition @'
using System;
namespace LmscMtpScanFixtures {
    public sealed class Folder {
        public object[] Children;
        public int Reads;
        public bool FailIfEnumerated;
        public Folder(object[] children) { Children = children; }
        public object[] Items() {
            ++Reads;
            if (FailIfEnumerated) throw new InvalidOperationException("Song resource subtree was scanned");
            return Children;
        }
    }
    public sealed class Item {
        public string Name;
        public bool IsFolder;
        public Folder GetFolder;
        public Item(string name, bool folder, Folder contents) {
            Name = name; IsFolder = folder; GetFolder = contents;
        }
    }
}
'@

function New-TestFolder {
    param([object[]]$Children = @())
    return [LmscMtpScanFixtures.Folder]::new($Children)
}
function New-TestItem {
    param([string]$Name, $Folder = $null)
    return [LmscMtpScanFixtures.Item]::new($Name, ($null -ne $Folder), $Folder)
}
function New-TestSongs {
    return ,(New-Object 'System.Collections.Generic.List[object]')
}
function Assert-Test {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw ('失败：' + $Message) }
}
function Assert-ScanFailure {
    param([scriptblock]$Action, [string]$Expected, [string]$Message)
    $records = New-Object 'System.Collections.Generic.List[object]'
    $exceptionText = ''
    try { & $Action | ForEach-Object { [void]$records.Add($_) } }
    catch { $exceptionText = $_.Exception.Message }
    Assert-Test ($exceptionText.Contains($Expected)) ($Message + '：必须抛出明确错误')
    Assert-Test ($records.Count -eq 0) ($Message + '：失败不能返回已完成扫描记录')
}
function Invoke-TestScan {
    param($Folder, $Source, $Songs, $Counters)
    Find-CategorizedSongs $Folder @() 0 $Source $script:testDevice $script:testStorage $Songs $Counters
}
function New-DepthChain {
    param([int]$SongDepth)
    $current = New-TestFolder @((New-TestItem 'Info.dat'))
    for ($level = $SongDepth; $level -ge 1; --$level) {
        $name = if ($level -eq $SongDepth) { '边界歌曲' } else { '分类-' + $level }
        $current = New-TestFolder @((New-TestItem $name $current))
    }
    return $current
}

$script:testDevice = New-TestItem 'PICO Neo 3' (New-TestFolder)
$script:testStorage = New-TestItem '内部共享存储空间' (New-TestFolder)
$sources = @(Get-SongSources)
Assert-Test ($sources.Count -eq 2) '两款游戏来源仍在白名单中'
$oasis = $sources | Where-Object { $_.GameId -eq 'oasis' }
$lightband = $sources | Where-Object { $_.GameId -eq 'lightband' }
Assert-Test (($oasis.Segments -join '/') -ceq 'SoulTopia/BeatNote/Custom') '星穹绿洲根路径正确'
Assert-Test (($lightband.Segments -join '/') -ceq 'Android/data/com.StarRiverVR.LightBand/files/CustomMusic') '光之乐团根路径正确'

# 同名歌曲在不同分类中各有独立定位；Info.dat 命中后不再把资源子目录当歌扫描。
$resourceTrap = New-TestFolder
$resourceTrap.FailIfEnumerated = $true
$songFolder = New-TestFolder @((New-TestItem 'iNfO.dAt'), (New-TestItem '资源' $resourceTrap))
$firstCategory = New-TestFolder @((New-TestItem '同名歌' $songFolder))
$secondCategory = New-TestFolder @((New-TestItem '同名歌' $songFolder))
$classified = New-TestFolder @((New-TestItem '电子' $firstCategory), (New-TestItem '练习' $secondCategory))
$songs = New-TestSongs
$counters = @{ Songs = 0; Directories = 0 }
Invoke-TestScan $classified $lightband $songs $counters
Assert-Test ($songs.Count -eq 2 -and $counters.Songs -eq 2) '不同分类的同名歌曲都保留'
Assert-Test (($songs[0].categorySegments -join '/') -ceq '电子') '第一首分类信息保留'
Assert-Test (($songs[1].categorySegments -join '/') -ceq '练习') '第二首分类信息保留'
Assert-Test ($songs[0].location -cne $songs[1].location) '同名歌曲完整位置可区分'
Assert-Test ((Test-SupportedLocator $songs[0].locator) -and (Test-SupportedLocator $songs[1].locator)) '扫描产生的多层定位可用于安全导入'
Assert-Test ($resourceTrap.Reads -eq 0) '找到 Info.dat 后停止资源子目录扫描'

# 空游戏根仍能被实际根路径查找定位，扫描得到零歌曲，而非“未找到游戏”。
foreach ($source in $sources) {
    $emptyRoot = New-TestFolder
    $current = $emptyRoot
    for ($level = $source.Segments.Count - 1; $level -ge 0; --$level) {
        $current = New-TestFolder @((New-TestItem $source.Segments[$level] $current))
    }
    $storage = New-TestItem '内部共享存储空间' $current
    $found = Find-SongRootFolder $storage $source.Segments
    Assert-Test ([object]::ReferenceEquals($found, $emptyRoot)) '空游戏根可被保留展示'
    $songs = New-TestSongs
    $counters = @{ Songs = 0; Directories = 0 }
    Invoke-TestScan $found $source $songs $counters
    Assert-Test ($songs.Count -eq 0 -and $counters.Directories -eq 1) '空游戏根完整扫描为零歌曲'
}

$songs = New-TestSongs
$counters = @{ Songs = 0; Directories = 0 }
Invoke-TestScan (New-DepthChain 20) $oasis $songs $counters
Assert-Test ($songs.Count -eq 1 -and $songs[0].categorySegments.Count -eq 19) '第20层歌曲允许扫描'
Assert-Test (Test-SupportedLocator $songs[0].locator) '最大允许深度的定位仍有效'
$songs = New-TestSongs
$counters = @{ Songs = 0; Directories = 0 }
Assert-ScanFailure { Invoke-TestScan (New-DepthChain 21) $oasis $songs $counters } '20' '第21层明确拒绝'

# 使用真实循环触达上限，避免只测试预填计数器与实现常量。
$basicSong = New-TestFolder @((New-TestItem 'Info.dat'))
$songItems = New-Object 'System.Collections.Generic.List[object]'
for ($index = 1; $index -le 4000; ++$index) { [void]$songItems.Add((New-TestItem ('歌曲-' + $index) $basicSong)) }
$songs = New-TestSongs
$counters = @{ Songs = 0; Directories = 0 }
Invoke-TestScan (New-TestFolder $songItems.ToArray()) $oasis $songs $counters
Assert-Test ($songs.Count -eq 4000 -and $counters.Songs -eq 4000) '4000首允许完整扫描'
[void]$songItems.Add((New-TestItem '歌曲-4001' $basicSong))
$songs = New-TestSongs
$counters = @{ Songs = 0; Directories = 0 }
Assert-ScanFailure { Invoke-TestScan (New-TestFolder $songItems.ToArray()) $oasis $songs $counters } '4000' '4001首禁止作为完整列表返回'
Assert-Test ($songs.Count -eq 4000 -and $counters.Songs -eq 4000) '拒绝超限前只保留内部部分结果'

$emptyFolder = New-TestFolder
$directoryItems = New-Object 'System.Collections.Generic.List[object]'
for ($index = 1; $index -le 9999; ++$index) { [void]$directoryItems.Add((New-TestItem ('分类-' + $index) $emptyFolder)) }
$songs = New-TestSongs
$counters = @{ Songs = 0; Directories = 0 }
Invoke-TestScan (New-TestFolder $directoryItems.ToArray()) $oasis $songs $counters
Assert-Test ($counters.Directories -eq 10000 -and $songs.Count -eq 0) '含根共10000个目录允许扫描'
[void]$directoryItems.Add((New-TestItem '分类-10000' $emptyFolder))
$songs = New-TestSongs
$counters = @{ Songs = 0; Directories = 0 }
Assert-ScanFailure { Invoke-TestScan (New-TestFolder $directoryItems.ToArray()) $oasis $songs $counters } '10000' '10001个目录禁止作为完整列表返回'

$badInfo = New-TestFolder @((New-TestItem 'Info.dat'), (New-TestItem 'INFO.DAT'))
$songs = New-TestSongs
$counters = @{ Songs = 0; Directories = 0 }
Assert-ScanFailure { Invoke-TestScan (New-TestFolder @((New-TestItem '重复Info' $badInfo))) $oasis $songs $counters } '多个 Info.dat' '重复Info拒绝定位'
Assert-ScanFailure { Invoke-TestScan $basicSong $oasis $songs $counters } '游戏根目录' '游戏根不能直接作为歌曲'
Assert-ScanFailure { Invoke-TestScan (New-TestFolder @((New-TestItem '..' $emptyFolder))) $oasis $songs $counters } '无法安全复制' '非法分类名称拒绝扫描'

$validSegments = @('内部共享存储空间') + $oasis.Segments + @('分类', '歌曲')
$valid = @{ device = 'PICO Neo 3'; segments = $validSegments }
Assert-Test (Test-SupportedLocator $valid) '合法分类定位通过'
$invalidLocators = @(
    @{ device = ''; segments = $validSegments },
    @{ device = 'PICO Neo 3'; segments = @('内部共享存储空间') + $oasis.Segments },
    @{ device = 'PICO Neo 3'; segments = @('内部共享存储空间', 'SoulTopia', 'Other', 'Custom', '歌曲') },
    @{ device = 'PICO Neo 3'; segments = @('内部共享存储空间', 'soultopia', 'BeatNote', 'Custom', '歌曲') },
    @{ device = 'PICO Neo 3'; segments = @('内部共享存储空间') + $oasis.Segments + @('..', '歌曲') },
    @{ device = 'PICO Neo 3'; segments = @('内部共享存储空间') + $oasis.Segments + @('非法:分类', '歌曲') },
    @{ device = 'PICO Neo 3'; segments = @('内部共享存储空间') + $oasis.Segments + @('CON', '歌曲') },
    @{ device = 'PICO Neo 3'; segments = @('内部共享存储空间') + $oasis.Segments + @('', '歌曲') },
    @{ device = 'PICO Neo 3'; segments = @('内部共享存储空间') + $oasis.Segments + @(12, '歌曲') },
    @{ device = 'PICO Neo 3'; segments = @('内部共享存储空间') + $oasis.Segments + (1..21 | ForEach-Object { '层-' + $_ }) }
)
foreach ($locator in $invalidLocators) { Assert-Test (-not (Test-SupportedLocator $locator)) '非法、越界或未知游戏定位被拒绝' }
$duplicated = New-TestFolder @((New-TestItem '歌曲' $basicSong), (New-TestItem '歌曲' $basicSong))
Assert-ScanFailure { Find-UniqueItem $duplicated '歌曲' } '名称重复' '同一父目录重复名称禁止用于导入'

Write-Output '通过：真实MTP扫描函数的分类定位、同名歌曲、资源停止、空游戏目录、20层/4000首/10000目录边界及非法定位；未访问设备。'
