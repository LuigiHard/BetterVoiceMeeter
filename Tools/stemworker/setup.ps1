# Installs the Sing Studio AI engine into %LOCALAPPDATA%\BetterVoiceMeeter\stems-env:
# PyTorch (CUDA 12.6 build: still supports GTX 10-series / Pascal), audio-separator, yt-dlp, ffmpeg.
$ErrorActionPreference = 'Stop'
$envDir = Join-Path $env:LOCALAPPDATA 'BetterVoiceMeeter\stems-env'

$python = (Get-Command py -ErrorAction SilentlyContinue)
$pyArgs = @('-3')
if (-not $python) { $python = Get-Command python -ErrorAction SilentlyContinue; $pyArgs = @() }
if (-not $python) { throw 'Python 3.10+ is required: install it from https://www.python.org/downloads/ (tick "Add to PATH").' }

Write-Output "Creating environment in $envDir"
& $python.Source @pyArgs -m venv $envDir
$py = Join-Path $envDir 'Scripts\python.exe'
& $py -m pip install --upgrade pip
if ($LASTEXITCODE) { throw 'pip upgrade failed' }

$hasNvidia = [bool](Get-Command nvidia-smi -ErrorAction SilentlyContinue)
if ($hasNvidia) {
    Write-Output 'Installing PyTorch (CUDA 12.6)...'
    & $py -m pip install torch torchaudio --index-url https://download.pytorch.org/whl/cu126
    if ($LASTEXITCODE) { throw 'PyTorch install failed' }
    & $py -m pip install 'audio-separator[gpu]' audioread yt-dlp static-ffmpeg
} else {
    Write-Output 'No NVIDIA GPU found: installing the CPU version (separation will be slower).'
    & $py -m pip install torch torchaudio
    & $py -m pip install 'audio-separator[cpu]' audioread yt-dlp static-ffmpeg
}
if ($LASTEXITCODE) { throw 'Package install failed' }

& $py -c "import static_ffmpeg; static_ffmpeg.add_paths(); import torch; print('torch', torch.__version__, 'GPU:', torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'none')"
Write-Output 'Done.'
