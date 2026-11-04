param([string]$PdsHome=$env:PDS_HOME)
$ErrorActionPreference='Stop'
if (!$PdsHome) { $PdsHome='E:/PDS_2022.2/PDS_2022.2-SP6.4' }
$projectRoot=Split-Path $PSScriptRoot -Parent
& "$PdsHome/bin/ip_generate.exe" -i "$projectRoot/vendor/sfp_hsst/sfp_hsst.idf" -disable-syn
if ($LASTEXITCODE -ne 0) { throw 'HSST generation failed' }
