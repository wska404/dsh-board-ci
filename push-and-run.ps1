$ErrorActionPreference = "Stop"

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    Write-Host "未安装 GitHub CLI。请先运行："
    Write-Host "  winget install --id GitHub.cli -e"
    exit 1
}

gh auth status 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "需要登录 GitHub..."
    gh auth login
}

$remote = git remote get-url origin 2>$null
if (-not $remote) {
    gh repo create dsh-board-ci --private --source=. --remote=origin --push
} else {
    git push -u origin main
}

gh workflow run firmware.yml
Start-Sleep -Seconds 5
gh run list --workflow firmware.yml --limit 1
gh run watch
