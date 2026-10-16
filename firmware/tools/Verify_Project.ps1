param([switch]$LinkedImage)
# 校验 .ioc、生成代码、工程文件和完整链接镜像，发现配置丢失时阻止交付。
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ioc = @{}
foreach ($line in [IO.File]::ReadAllLines((Join-Path $root 'XIAOLI-CODE.ioc'))) {
    if ($line.StartsWith('#') -or !$line.Contains('=')) { continue }
    $pair = $line.Split('=', 2)
    $ioc[$pair[0]] = $pair[1]
}
$required = @{
    'Mcu.UserName' = 'STM32F407ZGTx'; 'RCC.HSE_VALUE' = '8000000';
    'RCC.PLLM' = '8'; 'RCC.PLLN' = '336'; 'RCC.PLLQ' = '7';
    'RCC.PLLSourceVirtual' = 'RCC_PLLSOURCE_HSE'; 'RCC.SYSCLKFreq_VALUE' = '168000000';
    'PA9.Signal' = 'USART1_TX'; 'PA10.Signal' = 'USART1_RX';
    'PD5.Signal' = 'USART2_TX'; 'PD6.Signal' = 'USART2_RX';
    'PD8.Signal' = 'USART3_TX'; 'PD9.Signal' = 'USART3_RX';
    'PB5.GPIO_Label' = 'ALARM_RED'; 'PB4.GPIO_Label' = 'ALARM_YELLOW';
    'PB3.GPIO_Label' = 'ALARM_GREEN'; 'PB6.GPIO_Label' = 'ACTUATOR_IN1';
    'PB7.GPIO_Label' = 'ACTUATOR_IN2'; 'PB8.GPIO_Label' = 'BRUSH_MOTOR';
    'PD11.GPIO_Label' = 'SENSOR_POWER_1'; 'PD10.GPIO_Label' = 'SENSOR_POWER_2';
    'PG13.GPIO_Label' = 'RELAY_1'; 'PG12.GPIO_Label' = 'RELAY_2';
    'PD4.Signal' = 'GPIO_Input'; 'ProjectManager.KeepUserCode' = 'true'
    'ProjectManager.StackSize' = '0x1000'
    'Xiaoli.ApplicationFlashSize' = '0x000E0000'
    'Xiaoli.ConfigFlashBase' = '0x080E0000'
    'Xiaoli.ConfigFlashSize' = '0x00020000'
}
foreach ($pin in @('PE2', 'PE3', 'PB3', 'PB4', 'PB5', 'PB8', 'PD10', 'PD11', 'PG12', 'PG13')) {
    $required["$pin.PinState"] = 'GPIO_PIN_SET'
}
foreach ($pin in @('PB6', 'PB7')) { $required["$pin.PinState"] = 'GPIO_PIN_RESET' }
foreach ($n in 1..3) {
    $required["USART$n.WordLength"] = 'WORDLENGTH_8B'
    $required["USART$n.StopBits"] = 'STOPBITS_1'
    $required["USART$n.Parity"] = 'PARITY_NONE'
    $required["USART$n.BaudRate"] = $(if ($n -eq 3) { '9600' } else { '115200' })
}
foreach ($key in $required.Keys) {
    if ($ioc[$key] -ne $required[$key]) { throw "IOC mismatch: $key = $($ioc[$key])" }
}
$main = [IO.File]::ReadAllText((Join-Path $root 'Core/Src/main.c'))
foreach ($statement in @('RCC_PLLSOURCE_HSE', 'PLL.PLLM = 8;', 'PLL.PLLN = 336;',
    'PLL.PLLP = RCC_PLLP_DIV2;', 'PLL.PLLQ = 7;', 'Application_Init();', 'Application_Task();')) {
    if (!$main.Contains($statement)) { throw "Generated main missing: $statement" }
}
$startup = [IO.File]::ReadAllText((Join-Path $root 'MDK-ARM/startup_stm32f407xx.s'))
if ($startup -notmatch '(?m)^Stack_Size\s+EQU\s+0x1000\b') {
    throw 'Startup stack must be 0x1000 bytes.'
}
$uart = [IO.File]::ReadAllText((Join-Path $root 'Core/Src/usart.c'))
foreach ($n in 1..3) {
    foreach ($statement in @("huart$n.Init.WordLength = UART_WORDLENGTH_8B;",
        "huart$n.Init.StopBits = UART_STOPBITS_1;", "huart$n.Init.Parity = UART_PARITY_NONE;",
        "HAL_NVIC_EnableIRQ(USART${n}_IRQn);")) {
        if (!$uart.Contains($statement)) { throw "Generated UART missing: $statement" }
    }
}
[xml]$project = [IO.File]::ReadAllText((Join-Path $root 'MDK-ARM/XIAOLI-CODE.uvprojx'))
$flashRegions = $project.SelectNodes('//TargetArmAds/ArmAdsMisc/OnChipMemories/IROM | //TargetArmAds/ArmAdsMisc/OnChipMemories/OCR_RVCT4')
if ($flashRegions.Count -ne 2) { throw 'Keil Flash region nodes missing.' }
foreach ($memory in $flashRegions) {
    if ($memory.StartAddress -eq '0x8000000' -and [Convert]::ToUInt32($memory.Size, 16) -ne 0xE0000) {
        throw 'Application Flash must end before reserved sector 11 at 0x080E0000.'
    }
}
if ($project.SelectSingleNode('//Cads/VariousControls/IncludePath').InnerText -notmatch 'ThirdParty/jsmn') {
    throw 'Official jsmn include path missing.'
}
if ($project.SelectNodes('//Groups/Group/Files/File[not(FileName) and not(FilePath)]').Count -ne 0) {
    throw 'Empty Keil file placeholders.'
}
$sources = @($project.SelectNodes('//Groups/Group/Files/File[FileType="1"]/FilePath') | ForEach-Object {
    [IO.Path]::GetFullPath((Join-Path (Join-Path $root 'MDK-ARM') $_.InnerText))
})
if (($sources | Select-Object -Unique).Count -ne $sources.Count) { throw 'Duplicate compiled sources.' }
foreach ($item in Get-ChildItem -LiteralPath (Join-Path $root 'Application/Src') -Filter '*.c') {
    if ($sources -notcontains $item.FullName) { throw "Source not in Keil: $($item.Name)" }
}
foreach ($file in $sources) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Invalid Keil path: $file" }
}
Write-Output 'PASS: IOC, generated clocks/UARTs and Keil source membership.'
if ($LinkedImage) {
    $hex = Join-Path $root 'MDK-ARM/XIAOLI-CODE/XIAOLI-CODE.hex'
    $upper = [uint64]0
    $bytes = [uint64]0
    $end = [uint64]0
    foreach ($line in [IO.File]::ReadAllLines($hex)) {
        if ($line -notmatch '^:[0-9A-Fa-f]+$' -or ($line.Length % 2) -ne 1) {
            throw 'Malformed Intel HEX record.'
        }
        $count = [Convert]::ToUInt32($line.Substring(1, 2), 16)
        if ($line.Length -ne 11 + 2 * $count) { throw 'Intel HEX length mismatch.' }
        $sum = 0
        for ($i = 1; $i -lt $line.Length; $i += 2) { $sum += [Convert]::ToUInt32($line.Substring($i, 2), 16) }
        if (($sum % 256) -ne 0) { throw 'Intel HEX checksum mismatch.' }
        $kind = [Convert]::ToUInt32($line.Substring(7, 2), 16)
        if ($kind -eq 4) { $upper = [uint64][Convert]::ToUInt32($line.Substring(9, 4), 16) * 65536 }
        elseif ($kind -eq 2) { $upper = [uint64][Convert]::ToUInt32($line.Substring(9, 4), 16) * 16 }
        elseif ($kind -eq 0 -and $count -gt 0) {
            $address = $upper + [Convert]::ToUInt32($line.Substring(3, 4), 16)
            if ($address -lt 0x08000000 -or $address + $count -gt 0x080E0000) {
                throw ('Image overlaps reserved/outside Flash at 0x{0:X8}.' -f $address)
            }
            $bytes += $count
            $end = [Math]::Max($end, $address + $count)
        }
    }
    if ($bytes -eq 0) { throw 'Empty linked image.' }
    Write-Output ('PASS: Intel HEX {0} bytes; end 0x{1:X8} <= 0x080E0000.' -f $bytes, $end)
}
