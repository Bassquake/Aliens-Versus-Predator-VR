# Get the current directory where the script is executing
$currentDir = Get-Location

Write-Host "Starting robust lowercase conversion in: $currentDir" -ForegroundColor Cyan

# 1. Rename all FILES recursively
Get-ChildItem -Path $currentDir -Recurse -File | ForEach-Object {
    if ($_.Name -cne $_.Name.ToLower()) {
        $finalName = $_.Name.ToLower()
        
        # Create a temporary unique name in the same directory
        $tempName = "$($_.Name).lowercase_tmp"
        
        # Step 1: Rename to temporary name
        Rename-Item -LiteralPath $_.FullName -NewName $tempName -Force
        
        # Step 2: Rename from temporary to final lowercase name
        $tempPath = Join-Path $_.DirectoryName $tempName
        Rename-Item -LiteralPath $tempPath -NewName $finalName -Force
        
        Write-Host "Renamed File: $($_.Name) -> $finalName" -ForegroundColor Gray
    }
}

# 2. Rename all DIRECTORIES recursively (deepest first)
Get-ChildItem -Path $currentDir -Recurse -Directory | 
    Sort-Object {$_.FullName.Length} -Descending | 
    ForEach-Object {
        if ($_.Name -cne $_.Name.ToLower()) {
            $finalName = $_.Name.ToLower()
            
            # Create a temporary unique name
            $tempName = "$($_.Name)_lowercase_tmp"
            
            # Step 1: Rename folder to temporary name
            Rename-Item -LiteralPath $_.FullName -NewName $tempName -Force
            
            # Step 2: Rename from temporary to final lowercase name
            $parentDir = Split-Path $_.FullName -Parent
            $tempPath = Join-Path $parentDir $tempName
            Rename-Item -LiteralPath $tempPath -NewName $finalName -Force
            
            Write-Host "Renamed Folder: $($_.Name) -> $finalName" -ForegroundColor Yellow
        }
    }

Write-Host "Conversion complete!" -ForegroundColor Green