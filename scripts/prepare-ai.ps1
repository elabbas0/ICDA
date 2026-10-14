param([string]$LinuxRoot = '.verify/linuxroot', [switch]$IncludeLargerModel)
$ErrorActionPreference = 'Stop'
# Official Qwen release, pinned revision and LFS SHA-256 (no executable code).
$revision = '9217f5db79a29953eb74d5343926648285ec7e67'
$models = @(
    @{ Name='qwen2.5-0.5b-instruct-q4_k_m.gguf'; Repo='Qwen/Qwen2.5-0.5B-Instruct-GGUF'; Revision=$revision; Size=491400032; Sha='74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db' }
)
if ($IncludeLargerModel) {
    $models += @{ Name='qwen2.5-1.5b-instruct-q4_k_m.gguf'; Repo='Qwen/Qwen2.5-1.5B-Instruct-GGUF'; Revision='91cad51170dc346986eccefdc2dd33a9da36ead9'; Size=1117320736; Sha='6a1a2eb6d15622bf3c96857206351ba97e1af16c30d7a74ee38970e434e9407e' }
}
$dest = Join-Path $LinuxRoot 'usr/share/icda/models'
New-Item -ItemType Directory -Force $dest | Out-Null
foreach ($model in $models) {
    $path = Join-Path $dest $model.Name
    if (!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $model.Sha) {
        $partial = "$path.part"
        & curl.exe --fail --location --retry 3 --output $partial "https://huggingface.co/$($model.Repo)/resolve/$($model.Revision)/$($model.Name)"
        if ($LASTEXITCODE -ne 0) { throw 'Model download failed' }
        if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ne $model.Sha) { throw 'Model checksum mismatch' }
        Move-Item -LiteralPath $partial -Destination $path -Force
    }
    Write-Host "Verified $($model.Name)"
    $license = Join-Path $dest "$($model.Name).LICENSE"
    if (!(Test-Path -LiteralPath $license)) {
        & curl.exe --fail --location --retry 3 --output $license "https://huggingface.co/$($model.Repo)/resolve/$($model.Revision)/LICENSE"
        if ($LASTEXITCODE -ne 0) { throw 'Model license download failed' }
    }
}
$bin = Join-Path $LinuxRoot 'usr/bin'
New-Item -ItemType Directory -Force $bin | Out-Null
Copy-Item -LiteralPath 'userspace/llm/out/icda-llm' -Destination (Join-Path $bin 'icda-llm') -Force
$models | ConvertTo-Json | Set-Content -Encoding ASCII (Join-Path $dest 'sources.json')
Write-Host "AI bundle prepared in $LinuxRoot. Copy this Linux root to the ICDA system partition as /linux."
