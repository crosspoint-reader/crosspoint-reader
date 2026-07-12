<#
.SYNOPSIS
    Generate a preview stub for a biscuit. activity
    
.USAGE
    .\new-preview.ps1 MyNewActivity
    
    Adds a render function to test_preview.cpp and rebuilds.
#>

param(
    [Parameter(Mandatory=$true)]
    [string]$ActivityName
)

$env:PYTHONUTF8 = "1"
$previewFile = "test\test_preview\test_preview.cpp"
$funcName = "render_$($ActivityName.ToLower())"
$bmpName = "preview_$($ActivityName.ToLower()).bmp"

if (-not (Test-Path $previewFile)) {
    Write-Host "ERROR: $previewFile not found" -ForegroundColor Red
    exit 1
}

$content = Get-Content $previewFile -Raw

# Check if already exists
if ($content -match $funcName) {
    Write-Host "$funcName already exists in test_preview.cpp" -ForegroundColor Yellow
    Write-Host "Just edit it and run: pio test -e native -f test_preview" -ForegroundColor Cyan
    exit 0
}

# Find the source file for reference
$srcFile = "src\activities\apps\${ActivityName}Activity.cpp"
$hint = ""
if (Test-Path $srcFile) {
    Write-Host "Found source: $srcFile" -ForegroundColor Green
    $hint = "  // Reference: $srcFile"
} else {
    Write-Host "No source file found at $srcFile — creating blank template" -ForegroundColor Yellow
}

# Generate the stub
$stub = @"


void ${funcName}() {
${hint}
  renderer.clearScreen();
  drawHeader("${ActivityName}");

  // === YOUR RENDER CODE HERE ===
  // Copy drawing calls from your activity's render() method.
  // Available functions:
  //   renderer.drawCenteredText(UI_12_FONT_ID, y, "text", true, 1);  // bold
  //   renderer.drawCenteredText(UI_10_FONT_ID, y, "text");            // normal
  //   renderer.drawText(UI_10_FONT_ID, x, y, "text");
  //   renderer.drawText(SMALL_FONT_ID, x, y, "small text");
  //   renderer.fillRect(x, y, w, h, true);    // black
  //   renderer.fillRect(x, y, w, h, false);   // white
  //   renderer.drawRect(x, y, w, h);           // border
  //   renderer.drawLine(x1, y1, x2, y2);
  //   drawListItem(y, "Item", selected);       // list row
  //   drawDie(x, y, size, value);              // d6 die face
  //   drawHeader("Title", "subtitle");
  //
  // Screen: 480 wide x 800 tall
  // Header ends at y=42, button hints start at y=768

  renderer.drawCenteredText(UI_12_FONT_ID, 380, "TODO: add render code", true, 1);

  drawButtonHints("Back", "Select", "Up", "Down");

  TEST_ASSERT_TRUE(renderer.saveBMP("test/${bmpName}"));
}
"@

# Insert stub before the template comment block
$insertPoint = "// ============================================================`n// YOUR NEW ACTIVITY"
if ($content -match [regex]::Escape("// YOUR NEW ACTIVITY")) {
    $content = $content -replace [regex]::Escape("// ============================================================`n// YOUR NEW ACTIVITY"), "${stub}`n// ============================================================`n// YOUR NEW ACTIVITY"
} else {
    # Fallback: insert before setUp
    $content = $content -replace "// ============================================================\nvoid setUp", "${stub}`n// ============================================================`nvoid setUp"
}

# Add RUN_TEST line
$content = $content -replace "return UNITY_END\(\);", "  RUN_TEST(${funcName});`n  return UNITY_END();"

Set-Content $previewFile $content -NoNewline

Write-Host ""
Write-Host "Added $funcName to test_preview.cpp" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "  1. Edit test\test_preview\test_preview.cpp" -ForegroundColor White
Write-Host "     Find $funcName() and add your drawing code" -ForegroundColor White
Write-Host ""
Write-Host "  2. Preview:" -ForegroundColor White
Write-Host "     pio test -e native -f test_preview; start test\${bmpName}" -ForegroundColor Yellow
Write-Host ""
Write-Host "  Or use watch mode:" -ForegroundColor White
Write-Host "     .\preview.ps1" -ForegroundColor Yellow
