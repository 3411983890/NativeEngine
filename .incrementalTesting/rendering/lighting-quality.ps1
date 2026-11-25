param([switch]$SkipBuild, [switch]$SkipCaptures, [string]$Executable='build/lighting-quality/NativeGPU.exe', [string[]]$CaptureOnly=@())
$ErrorActionPreference='Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    if(-not $SkipBuild) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 -OutputDirectory (Split-Path $Executable -Parent)
        if($LASTEXITCODE -ne 0){throw 'Lighting build failed'}
    }
    & node tools/lighting-quality-fixtures.mjs
    if($LASTEXITCODE -ne 0){throw 'Lighting fixtures failed'}
    $cases=Get-Content reports/lighting-quality/cases.json -Raw | ConvertFrom-Json
    $runs=@{}
    foreach($case in $cases) {
        $report="reports/lighting-quality/$($case.id).json"
        $capture="reports/lighting-quality/$($case.id).png"
        if(-not $SkipCaptures -and ($CaptureOnly.Count -eq 0 -or $case.id -in $CaptureOnly)) {
            $arguments=@('--scene',$case.scene,'--textures','assets/textures','--smoke','--no-stress',
                '--frames',"$($case.frames)",'--render-size',$case.size,'--camera',$case.camera,
                '--screenshot',$capture,'--screenshot-frame',"$($case.shot)",'--report',$report,'--validation')
            if($case.view){$arguments+=@('--debug-view',$case.view)}
            if($case.cameraCut){$arguments+=@('--camera-cut',$case.cameraCut)}
            $process=Start-Process -FilePath $Executable -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Hidden -PassThru -Wait
            if($process.ExitCode -ne 0){throw "$($case.id) exited $($process.ExitCode)"}
        }
        $run=Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
        if(-not $run.success -or $run.probes.renderTargets.screenshots -lt 1 -or
           -not (Test-Path -LiteralPath $capture)){throw "$($case.id) failed: $($run.error)"}
        $quality=$run.probes.lightingQuality
        if(-not $quality.implemented -or $quality.error){throw "$($case.id): lighting path unavailable: $($quality.error)"}
        $messages=@($run.probes.renderTargets.validationMessages | Where-Object {$_ -match '\] [01]:'})
        if($messages.Count){throw "$($case.id): GPU validation errors: $messages"}
        $runs[$case.id]=$run
        Write-Host ("{0}: GPU P50 {1:N2} / P95 {2:N2} ms; DDGI {3:N2}, fog {4:N2}, clouds {5:N2} ms" -f `
            $case.id,$run.gpuP50Ms,$run.gpuP95Ms,$quality.gpuProbeMs,$quality.gpuFogMs,$quality.gpuCloudMs)
    }
    Add-Type -AssemblyName System.Drawing
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
public static class LightingImageMetrics {
    public static double[] Compare(string a,string b) {
        using(var x=new Bitmap(a))using(var y=new Bitmap(b)) {
            if(x.Width!=y.Width||x.Height!=y.Height)throw new Exception("Image sizes differ");
            double error=0,changed=0,deltaR=0,deltaG=0,deltaB=0;
            for(int j=0;j<x.Height;j++)for(int i=0;i<x.Width;i++) {
                var p=x.GetPixel(i,j);var q=y.GetPixel(i,j);
                int r=q.R-p.R,g=q.G-p.G,bv=q.B-p.B;
                error+=Math.Abs(r)+Math.Abs(g)+Math.Abs(bv);
                deltaR+=r;deltaG+=g;deltaB+=bv;
                if(Math.Max(Math.Abs(r),Math.Max(Math.Abs(g),Math.Abs(bv)))>2)changed++;
            }
            double count=x.Width*x.Height;
            return new[]{error/(count*3),changed/count,deltaR/count,deltaG/count,deltaB/count};
        }
    }
    public static double Patch(string path,int cx,int cy) {
        using(var image=new Bitmap(path)) {
            double sum=0;for(int y=cy-2;y<=cy+2;y++)for(int x=cx-2;x<=cx+2;x++)sum+=image.GetPixel(x,y).R;
            return sum/(25*255);
        }
    }
}
'@
    $checks=[System.Collections.Generic.List[object]]::new()
    function Compare-Pair([string]$id,[string]$a,[string]$b,[double]$minimum=0.05) {
        $metric=[LightingImageMetrics]::Compare((Join-Path $root "reports/lighting-quality/$a.png"),(Join-Path $root "reports/lighting-quality/$b.png"))
        $ok=$metric[0] -gt $minimum
        $checks.Add([ordered]@{id=$id;ok=$ok;meanAbsoluteError=$metric[0];changedFraction=$metric[1];deltaRGB=@($metric[2],$metric[3],$metric[4])})
        Write-Host "$id image difference: $($metric[0])"
    }
    Compare-Pair 'local surface occlusion' 'local-shadow-on' 'local-shadow-off'
    $clearOn=[LightingImageMetrics]::Patch((Join-Path $root 'reports/lighting-quality/local-unoccluded-on.png'),320,180)
    $clearOff=[LightingImageMetrics]::Patch((Join-Path $root 'reports/lighting-quality/local-unoccluded-off.png'),320,180)
    $blocked=[LightingImageMetrics]::Patch((Join-Path $root 'reports/lighting-quality/local-shadow-on.png'),320,180)
    $checks.Add([ordered]@{id='emitter shell does not occlude its own light';ok=([Math]::Abs($clearOn-$clearOff) -lt 0.03 -and $clearOff -gt 0.3);on=$clearOn;off=$clearOff})
    $checks.Add([ordered]@{id='blocker creates a localized shadow';ok=($clearOn-$blocked -gt 0.15);clear=$clearOn;blocked=$blocked})
    $cache=$runs['town-balanced'].probes.lightingQuality.staticShadowCache
    $checks.Add([ordered]@{id='static shadow cache survives moving traffic';ok=($cache.lights -eq 4 -and $cache.builds -eq 4);lights=$cache.lights;builds=$cache.builds})
    $tiny=$runs['local-minimum-range'].probes.lightingQuality.staticShadowCache
    $checks.Add([ordered]@{id='minimum accepted light range builds a valid cache';ok=($tiny.lights -eq 1 -and $tiny.builds -eq 1)})
    Compare-Pair 'independent volumetric occlusion' 'volume-shadow-on' 'volume-shadow-off'
    Compare-Pair 'directional light shafts' 'sun-occluded' 'sun-open'
    Compare-Pair 'sky diffuse and GGX IBL' 'ibl-off' 'ibl-on'
    Compare-Pair 'DDGI diffuse bounce' 'ddgi-off' 'ddgi-on'
    $bounce=@($checks | Where-Object {$_.id -eq 'DDGI diffuse bounce'})[0]
    $checks.Add([ordered]@{id='DDGI red color transfer';ok=($bounce.deltaRGB[0] -gt $bounce.deltaRGB[1]+0.05 -and $bounce.deltaRGB[0] -gt $bounce.deltaRGB[2]+0.05);deltaRGB=$bounce.deltaRGB})
    Compare-Pair 'cloud self shadow and scattering' 'cloud-on' 'cloud-off'
    Compare-Pair 'cloud casts ground shadow' 'cloud-ground-on' 'cloud-ground-off'
    $transmittance=[LightingImageMetrics]::Patch((Join-Path $root 'reports/lighting-quality/fog-uniform.png'),320,180)
    $expected=[Math]::Exp(-0.03*19.5)
    $checks.Add([ordered]@{id='homogeneous Beer-Lambert transmittance';ok=([Math]::Abs($transmittance-$expected) -lt 0.02);measured=$transmittance;expected=$expected;tolerance=0.02})
    $localT=[LightingImageMetrics]::Patch((Join-Path $root 'reports/lighting-quality/fog-local.png'),320,180)
    $checks.Add([ordered]@{id='localized density volume';ok=($localT -gt 0.75 -and $localT -lt 0.95);measured=$localT})
    $hashA=(Get-FileHash reports/lighting-quality/fog-uniform.png -Algorithm SHA256).Hash
    $hashB=(Get-FileHash reports/lighting-quality/repeat.png -Algorithm SHA256).Hash
    $checks.Add([ordered]@{id='deterministic repeat';ok=($hashA -eq $hashB);sha256=$hashA.ToLowerInvariant()})
    $townHash=(Get-FileHash reports/lighting-quality/town-balanced.png -Algorithm SHA256).Hash
    $townRepeat=(Get-FileHash reports/lighting-quality/town-repeat.png -Algorithm SHA256).Hash
    $checks.Add([ordered]@{id='full lighting deterministic repeat';ok=($townHash -eq $townRepeat);sha256=$townHash.ToLowerInvariant()})
    $cutHash=(Get-FileHash reports/lighting-quality/camera-cut.png -Algorithm SHA256).Hash
    $referenceHash=(Get-FileHash reports/lighting-quality/camera-cut-reference.png -Algorithm SHA256).Hash
    $resets=$runs['camera-cut'].probes.lightingQuality.historyResets
    $checks.Add([ordered]@{id='camera cut rejects history';ok=($cutHash -eq $referenceHash -and $resets -ge 2);historyResets=$resets})
    $performance=@($cases | ForEach-Object {
        $run=$runs[$_.id]
        [ordered]@{id=$_.id;size=$_.size;frames=$run.frames;gpuP50Ms=$run.gpuP50Ms;gpuP95Ms=$run.gpuP95Ms;passes=$run.probes.lightingQuality}
    })
    $failures=@($checks | Where-Object {-not $_.ok})
    [ordered]@{schema='nativegpu.lighting-quality/1';ok=($failures.Count -eq 0);
        adapter=$runs['town-balanced'].capabilities.adapter;validationLayer=$runs['town-balanced'].probes.renderTargets.validationLayer;
        checks=$checks;performance=$performance} | ConvertTo-Json -Depth 12 | Set-Content reports/lighting-quality/evidence.json -Encoding utf8
    if($failures.Count){throw "Lighting acceptance failed: $($failures.id -join ', ')"}
    Write-Host 'Lighting quality acceptance passed.'
} finally {Pop-Location}
