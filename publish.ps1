# Run from Windows PowerShell: .\publish.ps1 -Message "feat: describe the change"
[CmdletBinding()]
param(
    [string]$Message,
    [switch]$Preview
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# Handle native Git exit codes explicitly in both Windows PowerShell 5.1 and PowerShell 7.
$PSNativeCommandUseErrorActionPreference = $false

function Invoke-GitChecked {
    param([string[]]$GitArguments)
    & git @GitArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Git failed (exit $LASTEXITCODE). Stopped without resetting local work."
    }
}

$locationPushed = $false
try {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw 'Git is not available. Install Git for Windows first.'
    }
    Push-Location -LiteralPath $PSScriptRoot
    $locationPushed = $true
    $repoRoot = & git rev-parse --show-toplevel
    if ($LASTEXITCODE -ne 0) { throw 'Place publish.ps1 at the root of a Git repository.' }
    if ((Resolve-Path -LiteralPath $repoRoot).Path -ne (Resolve-Path -LiteralPath $PSScriptRoot).Path) {
        throw 'Place publish.ps1 at the repository root.'
    }
    $branch = & git symbolic-ref --quiet --short HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Detached HEAD: switch to a branch before publishing.' }
    $null = & git remote get-url --push origin
    if ($LASTEXITCODE -ne 0) { throw 'Configure the origin remote before publishing.' }
    $conflicts = @(Invoke-GitChecked -GitArguments @('ls-files', '--unmerged'))
    if ($conflicts.Count -gt 0) { throw 'Resolve merge conflicts before publishing.' }

    Write-Host "Repository: $repoRoot"
    Write-Host "Branch: $branch -> origin/$branch"
    Write-Host 'Changed files (all non-ignored changes will be included):'
    Invoke-GitChecked -GitArguments @('status', '--short')
    Invoke-GitChecked -GitArguments @('diff', '--stat', 'HEAD')

    if ($Preview) {
        Write-Host 'Preview only. No files staged, no commit created, no push performed.'
        exit 0
    }

    $changes = @(Invoke-GitChecked -GitArguments @('status', '--porcelain'))
    if ($changes.Count -gt 0) {
        if ([string]::IsNullOrWhiteSpace($Message)) { $Message = Read-Host 'Commit message (English)' }
        if ([string]::IsNullOrWhiteSpace($Message)) { throw 'A non-empty commit message is required.' }
        Write-Host "Commit message: $Message"
    } else {
        Write-Host 'No file changes. Existing local commits will be pushed.'
    }
    Write-Host 'This includes already staged files and all existing unpushed commits on this branch.'
    $answer = Read-Host 'Commit all listed changes and push to origin? [y/N]'
    if ($answer -notmatch '^(y|yes)$') {
        Write-Host 'Cancelled. No changes made.'
        exit 0
    }

    if ($changes.Count -gt 0) {
        Invoke-GitChecked -GitArguments @('add', '--all', '--', '.')
        & git diff --cached --quiet
        $diffExitCode = $LASTEXITCODE
        if ($diffExitCode -eq 1) {
            Invoke-GitChecked -GitArguments @('commit', '-m', $Message)
        } elseif ($diffExitCode -eq 0) {
            Write-Host 'No content changes after staging; skipping the commit.'
        } else {
            throw "Could not inspect staged changes (exit $diffExitCode)."
        }
    }
    & git push --set-upstream origin "HEAD:refs/heads/$branch"
    if ($LASTEXITCODE -ne 0) {
        throw 'Push failed. Local commits are preserved. Resolve the reported authentication or remote-history issue, then rerun this script. No force push was attempted.'
    }
    Write-Host 'Published successfully.'
} catch {
    Write-Error -Message $_.Exception.Message -ErrorAction Continue
    exit 1
} finally {
    if ($locationPushed) { Pop-Location }
}
