function Trim-DownloadCache {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,
        [ValidateRange(0, 9223372036854775807)]
        [long] $MaximumBytes = 128MB
    )

    [long] $retainedBytes = 0
    $retainedFiles = 0
    if (Test-Path -LiteralPath $Path -PathType Container) {
        $files = Get-ChildItem -LiteralPath $Path -File -Recurse -Force -ErrorAction Stop |
            Sort-Object -Property @{ Expression = 'LastWriteTimeUtc'; Descending = $true }, FullName
        foreach ($file in $files) {
            if ($file.Length -le ($MaximumBytes - $retainedBytes)) {
                $retainedBytes += $file.Length
                $retainedFiles++
            }
            else {
                Remove-Item -LiteralPath $file.FullName -Force -ErrorAction Stop
            }
        }
    }
    Write-Host ('Download cache: retained {0} files, {1:N1} MiB of {2:N1} MiB.' -f
        $retainedFiles, ($retainedBytes / 1MB), ($MaximumBytes / 1MB))
}
