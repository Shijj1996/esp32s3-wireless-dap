# Helper: enter the ESP-IDF v5.5.4 environment and forward all arguments to idf.py
$env:IDF_TOOLS_PATH      = 'C:\Espressif'
$env:IDF_PATH            = 'D:\ESP32_SDK\esp32s3_v554\.espressif\v5.5.4\esp-idf'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v5.5.4\venv'
$env:ESP_ROM_ELF_DIR      = 'C:\Espressif\tools\esp-rom-elfs\20241011\'

$env:PATH = @(
    'C:\Espressif\tools\python\v5.5.4\venv\Scripts'
    'C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin'
    'C:\Espressif\tools\cmake\3.30.2\bin'
    'C:\Espressif\tools\ninja\1.12.1'
    'C:\Espressif\tools\idf-exe\1.0.3'
) -join ';' | ForEach-Object { "$_;$env:PATH" }

& 'C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe' "$env:IDF_PATH\tools\idf.py" @args
exit $LASTEXITCODE
