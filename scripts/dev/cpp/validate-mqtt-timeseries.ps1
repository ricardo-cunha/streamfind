[CmdletBinding()]
param(
    [string]$ServiceEndpoint = 'http://127.0.0.1:8791',
    [string]$PublisherExecutable = 'C:\msys64\ucrt64\bin\mosquitto_pub.exe',
    [string]$BrokerHost = '127.0.0.1',
    [int]$BrokerPort = 18883,
    [switch]$KeepProject
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$projectRoot = Join-Path $repoRoot 'tmp\projects'
$project = Join-Path $projectRoot 'mqtt-timeseries-dev-check.sf'
$session = 'mqtt-timeseries-dev-check'
$logRoot = Join-Path $repoRoot 'tmp\logs'
$summaryPath = Join-Path $logRoot 'mqtt-timeseries-dev-check.summary.json'
$ServiceEndpoint = $ServiceEndpoint.TrimEnd('/')

if (-not (Test-Path -LiteralPath $PublisherExecutable)) {
    throw "MQTT publisher executable not found: $PublisherExecutable"
}
New-Item -ItemType Directory -Force -Path $projectRoot, $logRoot | Out-Null

function Invoke-JsonPost([string]$uri, [object]$body) {
    Invoke-RestMethod -Uri $uri -Method Post -ContentType 'application/json' `
        -Body ($body | ConvertTo-Json -Depth 40)
}

function Publish-Mqtt([string]$topic, [string]$message) {
    & $PublisherExecutable -h $BrokerHost -p $BrokerPort -q 1 -t $topic -m $message | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "MQTT publish failed for topic $topic ($LASTEXITCODE)" }
}

try {
    try { Invoke-RestMethod -Uri "$ServiceEndpoint/projects/$session" -Method Delete | Out-Null } catch { }
    if (Test-Path -LiteralPath $project) { Remove-Item -LiteralPath $project -Recurse -Force }

    Invoke-JsonPost "$ServiceEndpoint/projects" @{
        mode = 'create'; session_id = $session; project_path = $project
    } | Out-Null
    $current = Invoke-RestMethod -Uri "$ServiceEndpoint/projects/$session/workflow"
    $workflow = [ordered]@{
        name = 'MQTT sensor time-series dev check'
        schema_version = 2
        definition_revision = [int]$current.workflow.definition_revision
        workflow_id = 'mqtt-sensor-timeseries-dev-check'
        version = 1
        operations = @(
            [ordered]@{
                id = 1
                operation = 'sensors.capture_mqtt_timeseries'
                parameters = [ordered]@{
                    broker_endpoint = "tcp://$BrokerHost`:$BrokerPort"
                    topics = [ordered]@{
                        temperature = 'streamfind/dev/temperature'
                        pressure = 'streamfind/dev/pressure'
                    }
                    payload_format = 'json'
                    qos = 1
                    client_id = 'streamfind-mqtt-dev-check'
                    max_reconnect_attempts = 3
                    reconnect_backoff_seconds = 1
                    mqtt_username = ''
                    mqtt_password = ''
                    tls_enabled = $false
                    tls_verify_server = $true
                    capture_duration = 5
                    max_samples = 20
                    max_buffer_bytes = 1048576
                    capture_id = 'mqtt-dev-check-capture'
                }
                inputs = @{}
            }
        )
        connections = @()
    }
    Invoke-RestMethod -Uri "$ServiceEndpoint/projects/$session/workflow" -Method Post `
        -ContentType 'application/json' -Body ($workflow | ConvertTo-Json -Depth 40) | Out-Null
    $validation = Invoke-JsonPost "$ServiceEndpoint/projects/$session/workflow/validate" @{}
    if (-not $validation.valid) { throw "MQTT workflow validation failed: $($validation | ConvertTo-Json -Depth 20 -Compress)" }
    Invoke-JsonPost "$ServiceEndpoint/projects/$session/workflow/run" @{ execution_mode = 'detailed' } | Out-Null

    Start-Sleep -Milliseconds 1000
    Publish-Mqtt 'streamfind/dev/temperature' '{"value":24.5,"unit":"degC","event_time":"2026-11-19T14:00:00.123456Z","quality":"good"}'
    Publish-Mqtt 'streamfind/dev/pressure' '{"value":101.3,"unit":"kPa","event_time":"2026-11-19T14:00:00.223456Z","quality":"good"}'
    Publish-Mqtt 'streamfind/dev/temperature' '{"value":true,"unit":"alarm","event_time":"2026-11-19T14:00:01.123456Z","quality":"uncertain"}'
    Publish-Mqtt 'streamfind/dev/pressure' '{"value":null,"unit":"kPa","event_time":"2026-11-19T14:00:01.223456Z","quality":"missing"}'

    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 500
        $state = Invoke-RestMethod -Uri "$ServiceEndpoint/projects/$session/workflow/state"
    } while ($state.state -in @('queued', 'running', 'cancelling') -and [DateTime]::UtcNow -lt $deadline)
    if ($state.state -ne 'completed') { throw "MQTT workflow ended in state $($state.state)" }

    $results = (Invoke-RestMethod -Uri "$ServiceEndpoint/projects/$session/results").results
    $result = @($results | Where-Object {
        $_.contract_id -eq 'sensorTimeseriesTable' -and $_.status -eq 'published'
    } | Select-Object -Last 1)
    if ($result.Count -ne 1) { throw 'MQTT workflow did not publish one sensorTimeseriesTable result' }
    $data = Invoke-JsonPost "$ServiceEndpoint/projects/$session/results/query" @{
        result_id = $result[0].result_id; mode = 'detail'; offset = 0; limit = 100
    }
    $rows = @($data.rows)
    if ($rows.Count -ne 4) { throw "Expected 4 MQTT rows, got $($rows.Count)" }
    $summary = [ordered]@{
        service_endpoint = $ServiceEndpoint
        project = $project
        result_id = $result[0].result_id
        row_count = $rows.Count
        value_types = @($rows | ForEach-Object { $_.value_type } | Sort-Object -Unique)
        state = $state.state
    }
    $summary | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
    $summary | ConvertTo-Json -Depth 20
} finally {
    if (-not $KeepProject) {
        try { Invoke-RestMethod -Uri "$ServiceEndpoint/projects/$session" -Method Delete | Out-Null } catch { }
    }
}
