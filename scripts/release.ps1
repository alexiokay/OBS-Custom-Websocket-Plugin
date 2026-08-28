param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('patch', 'minor', 'major')]
    [string] $Bump
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Push-Location $repositoryRoot

function Invoke-Git {
    $gitArguments = @($args)
    & git @gitArguments
    if ($LASTEXITCODE -ne 0) {
        throw "git $($gitArguments -join ' ') failed with exit code $LASTEXITCODE"
    }
}

try {
    if ((& git branch --show-current).Trim() -ne 'main') {
        throw 'Companion releases must be created from the main branch.'
    }
    & git diff --cached --quiet
    if ($LASTEXITCODE -ne 0) {
        throw 'The Git index contains staged changes. Commit or unstage them before releasing.'
    }
    & git diff --quiet -- buildspec.json
    if ($LASTEXITCODE -ne 0) {
        throw 'buildspec.json already has uncommitted changes.'
    }

    Invoke-Git fetch origin main --tags
    $head = (& git rev-parse HEAD).Trim()
    $originMain = (& git rev-parse origin/main).Trim()
    if ($head -ne $originMain) {
        throw "Local main ($head) must exactly match origin/main ($originMain)."
    }

    $buildspecPath = Join-Path $repositoryRoot 'buildspec.json'
    $buildspec = Get-Content -Raw -LiteralPath $buildspecPath | ConvertFrom-Json
    if ($buildspec.version -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
        throw "buildspec.json contains an invalid semantic version: $($buildspec.version)"
    }
    $major = [int] $Matches[1]
    $minor = [int] $Matches[2]
    $patch = [int] $Matches[3]
    switch ($Bump) {
        'major' { $major++; $minor = 0; $patch = 0 }
        'minor' { $minor++; $patch = 0 }
        'patch' { $patch++ }
    }
    $nextVersion = "$major.$minor.$patch"
    $tag = "v$nextVersion"

    & git rev-parse --verify --quiet "refs/tags/$tag" | Out-Null
    if ($LASTEXITCODE -eq 0) {
        throw "Tag $tag already exists locally."
    }
    & git ls-remote --exit-code --tags origin "refs/tags/$tag" | Out-Null
    if ($LASTEXITCODE -eq 0) {
        throw "Tag $tag already exists on origin."
    }

    $content = Get-Content -Raw -LiteralPath $buildspecPath
    $escapedVersion = [Regex]::Escape([string] $buildspec.version)
    $pattern = '("version"\s*:\s*")' + $escapedVersion + '(")'
    $matches = [Regex]::Matches($content, $pattern)
    if ($matches.Count -ne 1) {
        throw "Expected exactly one version field with value $($buildspec.version); found $($matches.Count)."
    }
    $updated = [Regex]::Replace($content, $pattern, "`${1}$nextVersion`${2}", 1)
    Set-Content -LiteralPath $buildspecPath -Value $updated -Encoding utf8 -NoNewline

    & git diff --check -- buildspec.json
    if ($LASTEXITCODE -ne 0) {
        throw 'Version update introduced an invalid diff.'
    }
    Invoke-Git commit --only -m "Release OBS companion $tag" -- buildspec.json
    Invoke-Git tag -a $tag -m "VortiDeck OBS Companion $tag"
    Invoke-Git push origin main
    Invoke-Git push origin $tag

    Write-Host "Released source tag $tag. GitHub Actions will build and publish the companion."
    Write-Host "https://github.com/alexiokay/OBS-Custom-Websocket-Plugin/actions"
} finally {
    Pop-Location
}
