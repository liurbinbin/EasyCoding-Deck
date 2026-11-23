param(
  [string]$Port = "COM32",
  [switch]$SkipConfig,
  [switch]$PlayTone,
  [ValidateRange(20, 15000)]
  [int]$ToneDurationMs = 600
)

$ErrorActionPreference = "Stop"

function Write-PacedJson {
  param([System.IO.Ports.SerialPort]$Serial, [hashtable]$Message)
  $line = ($Message | ConvertTo-Json -Compress -Depth 12) + "`n"
  $bytes = [Text.Encoding]::UTF8.GetBytes($line)
  for ($offset = 0; $offset -lt $bytes.Length; $offset += 64) {
    $count = [Math]::Min(64, $bytes.Length - $offset)
    $Serial.Write($bytes, $offset, $count)
    Start-Sleep -Milliseconds 1
  }
}

function Read-Message {
  param(
    [System.IO.Ports.SerialPort]$Serial,
    [string]$Topic,
    [string]$RequestId,
    [int]$TimeoutMs = 4000
  )
  $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
  while ([DateTime]::UtcNow -lt $deadline) {
    try {
      $line = $Serial.ReadLine().Trim()
    } catch [TimeoutException] {
      continue
    }
    if (-not $line.StartsWith("{")) { continue }
    try { $message = $line | ConvertFrom-Json } catch { continue }
    if ($message.topic -ne $Topic) { continue }
    if ($RequestId -and $message.payload.requestId -ne $RequestId) { continue }
    return $message
  }
  throw "Timed out waiting for $Topic ($RequestId)"
}

function New-TonePcm {
  param([int]$DurationMs = 600, [double]$Frequency = 523.25)
  $samples = [Math]::Floor(16000 * $DurationMs / 1000)
  $pcm = [byte[]]::new($samples * 2)
  for ($index = 0; $index -lt $samples; $index++) {
    $sample = [int16]([Math]::Sin(2 * [Math]::PI * $Frequency * $index / 16000) * 12000)
    $pcm[$index * 2] = [byte]($sample -band 0xff)
    $pcm[$index * 2 + 1] = [byte](($sample -shr 8) -band 0xff)
  }
  return $pcm
}

$serial = [System.IO.Ports.SerialPort]::new($Port, 115200, "None", 8, "One")
$serial.NewLine = "`n"
$serial.ReadTimeout = 150
$serial.WriteTimeout = 2000
$serial.DtrEnable = $true
$serial.RtsEnable = $true

try {
  $serial.Open()
  Start-Sleep -Milliseconds 250
  $serial.DiscardInBuffer()

  if (-not $SkipConfig) {
    $queryId = "smoke-query-$([Guid]::NewGuid().ToString('N'))"
    Write-PacedJson $serial @{ topic = "input/config-query"; payload = @{ requestId = $queryId } }
    $state = Read-Message $serial "input/config-state" $queryId
    if ($state.payload.config.bindings.Count -ne 15) {
      throw "Expected 15 bindings, got $($state.payload.config.bindings.Count)"
    }

    $writeId = "smoke-write-$([Guid]::NewGuid().ToString('N'))"
    Write-PacedJson $serial @{ topic = "input/config"; payload = @{ requestId = $writeId; bindings = $state.payload.config.bindings } }
    $ack = Read-Message $serial "input/config-ack" $writeId
    if (-not $ack.payload.ok -or -not $ack.payload.persisted -or $ack.payload.bindingCount -ne 15) {
      throw "Full input configuration was not acknowledged and persisted"
    }
    Write-Host "PASS input/config: 15 bindings acknowledged and persisted"
  }

  $volumeId = "smoke-volume-$([Guid]::NewGuid().ToString('N'))"
  Write-PacedJson $serial @{ topic = "audio/volume"; payload = @{ requestId = $volumeId; operation = "set"; volume = 75 } }
  $volume = Read-Message $serial "audio/volume-state" $volumeId
  if (-not $volume.payload.ok -or $volume.payload.volume -ne 75) {
    throw "Volume command was rejected"
  }
  Write-Host "PASS audio/volume: 75%, persisted=$($volume.payload.persisted)"

  if ($PlayTone) {
    $session = "smoke-tone-$([Guid]::NewGuid().ToString('N'))"
    $beginId = "smoke-begin-$([Guid]::NewGuid().ToString('N'))"
    $pcm = New-TonePcm -DurationMs $ToneDurationMs
    Write-PacedJson $serial @{ topic = "audio/play_begin"; payload = @{ requestId = $beginId; sessionId = $session; sampleRate = 16000; bytes = $pcm.Length } }
    $begin = Read-Message $serial "audio/play-state" $beginId
    if (-not $begin.payload.ok) { throw "Playback start was rejected: $($begin.payload.error)" }

    $sequence = 0
    for ($offset = 0; $offset -lt $pcm.Length; $offset += 512) {
      $count = [Math]::Min(512, $pcm.Length - $offset)
      $chunk = [byte[]]::new($count)
      [Array]::Copy($pcm, $offset, $chunk, 0, $count)
      $chunkId = "smoke-chunk-$sequence-$([Guid]::NewGuid().ToString('N'))"
      Write-PacedJson $serial @{ topic = "audio/play_chunk"; payload = @{ requestId = $chunkId; sessionId = $session; seq = $sequence; bytes = $count; data = [Convert]::ToBase64String($chunk) } }
      $chunkAck = Read-Message $serial "protocol/ack" $chunkId
      if (-not $chunkAck.payload.ok) { throw "Audio chunk $sequence was rejected" }
      Start-Sleep -Milliseconds ([Math]::Ceiling($count * 2000 / 32000 + 8))
      $sequence++
    }
    $endId = "smoke-end-$([Guid]::NewGuid().ToString('N'))"
    Write-PacedJson $serial @{ topic = "audio/play_end"; payload = @{ requestId = $endId; sessionId = $session } }
    $endAck = Read-Message $serial "protocol/ack" $endId
    if (-not $endAck.payload.ok) { throw "Playback end was rejected" }
    Write-Host "PASS audio/playback: $($pcm.Length) PCM bytes acknowledged; playback ended"
  }
} finally {
  if ($serial.IsOpen) { $serial.Close() }
  $serial.Dispose()
}
