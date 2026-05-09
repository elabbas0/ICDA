function Get-DockerCli {
    $cmd = Get-Command docker -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    $programFilesDocker = "C:\Program Files\Docker\Docker\resources\bin\docker.exe"
    if (Test-Path $programFilesDocker) {
        return $programFilesDocker
    }

    throw "Docker CLI was not found. Run scripts\check-requirements.cmd -NoWsl, then start Docker Desktop."
}

function Invoke-Docker {
    param(
        [Parameter(ValueFromRemainingArguments = $true)]
        [object[]]$Arguments
    )

    $docker = Get-DockerCli
    if ($Arguments.Count -eq 1 -and $Arguments[0] -is [array]) {
        $Arguments = $Arguments[0]
    }

    $dockerArgs = @($Arguments | ForEach-Object { [string]$_ })
    & $docker @dockerArgs
    if ($LASTEXITCODE -ne 0) {
        throw "docker $($dockerArgs -join ' ') failed with exit code $LASTEXITCODE"
    }
}
