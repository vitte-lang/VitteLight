function Show-Tree([string]$Path='.', [int]$Depth=0, [int]$MaxDepth=99, [string[]]$Ignore=@('.git','target','node_modules','.vscode','dist','.idea')) {
  if ($Depth -eq 0) { '```'; }
  Get-ChildItem -LiteralPath $Path -Force | Where-Object { -not ($Ignore -contains $_.Name) } | Sort-Object { -not $_.PSIsContainer }, Name | ForEach-Object {
    $indent = ('│   ' * $Depth)
    $prefix = if ($_.PSIsContainer) { '├── ' } else { '├── ' }
    "$indent$prefix$($_.Name)"
    if ($_.PSIsContainer -and $Depth -lt $MaxDepth) { Show-Tree -Path $_.FullName -Depth ($Depth+1) -MaxDepth $MaxDepth -Ignore $Ignore }
  }
  if ($Depth -eq 0) { '```' }
}
Show-Tree | Out-File -Encoding utf8 docs\arborescence.md
