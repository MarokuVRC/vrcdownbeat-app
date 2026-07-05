# Creates a fake recorded-jam folder from an existing library song, so the
# Recordings tab can be tested without running a full jam.
$ErrorActionPreference = 'Stop'

$lib = Join-Path $env:APPDATA 'BandJam\library\Pink - Just Like a Pill_25334c42'
$rec = Join-Path $env:APPDATA ('BandJam\recordings\Pink - Just Like a Pill_' + (Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'))
New-Item -ItemType Directory -Path $rec -Force | Out-Null

$songJson = Get-Content (Join-Path $lib 'song.json') -Raw | ConvertFrom-Json
$rate = if ($songJson.sampleRate) { $songJson.sampleRate } else { 44100 }

$tracks = @()
$i = 0
foreach ($f in Get-ChildItem $lib -File | Where-Object { $_.Name -ne 'song.json' }) {
    $i++
    $name = $f.BaseName -replace '^.*\(', '' -replace '\).*$', ''
    $dest = ('stem_{0:d2}_{1}{2}' -f $i, $name, $f.Extension)
    Copy-Item $f.FullName (Join-Path $rec $dest)
    $kind = 'stem'
    if ($i -eq 4) { $kind = 'musician' }
    if ($i -eq 5) { $kind = 'host' }
    $tracks += [ordered]@{ file = $dest; name = $name; kind = $kind }
}

$meta = [ordered]@{
    song          = 'Pink - Just Like a Pill (test)'
    date          = (Get-Date -Format 'yyyy-MM-dd HH:mm')
    sampleRate    = $rate
    lengthSamples = [int64]($rate * 215)
    tracks        = $tracks
}
$meta | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $rec 'meta.json') -Encoding UTF8
Write-Output "created: $rec ($($tracks.Count) tracks)"
