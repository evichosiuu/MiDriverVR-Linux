<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<title>Guía de compilación — MiDriverVR</title>
<style>
  :root{
    --bg:#0f1115;
    --panel:#161922;
    --border:#262b38;
    --text:#e7e9ee;
    --muted:#9aa3b2;
    --accent:#6ea8fe;
    --accent2:#8fd6c4;
    --code-bg:#0b0d12;
  }
  *{box-sizing:border-box;}
  body{
    margin:0;
    background:var(--bg);
    color:var(--text);
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
    line-height:1.6;
  }
  .wrap{max-width:820px;margin:0 auto;padding:32px 20px 80px;}
  .toggle-bar{
    position:sticky;top:0;
    background:rgba(15,17,21,0.92);
    backdrop-filter:blur(6px);
    padding:14px 0;
    display:flex;justify-content:flex-end;gap:8px;
    border-bottom:1px solid var(--border);
    z-index:10;
  }
  .toggle-inner{max-width:820px;margin:0 auto;padding:0 20px;display:flex;justify-content:flex-end;gap:8px;}
  button.lang-btn{
    background:var(--panel);
    color:var(--muted);
    border:1px solid var(--border);
    padding:8px 16px;
    border-radius:8px;
    font-size:14px;
    cursor:pointer;
    font-weight:600;
    transition:all .15s ease;
  }
  button.lang-btn.active{
    background:var(--accent);
    color:#0f1115;
    border-color:var(--accent);
  }
  h1{font-size:26px;margin-top:8px;color:#fff;border-bottom:2px solid var(--accent);padding-bottom:10px;}
  h2{font-size:20px;color:var(--accent2);margin-top:36px;}
  h3{font-size:16px;color:var(--text);margin-top:24px;}
  p{color:var(--text);}
  ul,ol{padding-left:22px;}
  li{margin-bottom:6px;}
  code{
    background:var(--code-bg);
    border:1px solid var(--border);
    padding:2px 6px;
    border-radius:4px;
    font-size:0.9em;
    color:var(--accent2);
  }
  pre{
    background:var(--code-bg);
    border:1px solid var(--border);
    padding:14px 16px;
    border-radius:8px;
    overflow-x:auto;
    font-size:13.5px;
    color:#d7dae2;
  }
  pre code{background:none;border:none;padding:0;color:inherit;}
  table{border-collapse:collapse;width:100%;margin:14px 0;}
  th,td{border:1px solid var(--border);padding:8px 12px;text-align:left;font-size:14px;}
  th{background:var(--panel);color:var(--accent2);}
  hr{border:none;border-top:1px solid var(--border);margin:32px 0;}
  a{color:var(--accent);}
  blockquote{
    border-left:3px solid var(--accent);
    margin:12px 0;
    padding:6px 14px;
    color:var(--muted);
    background:var(--panel);
    border-radius:0 6px 6px 0;
  }
  .lang{display:none;}
  .lang.active{display:block;}
</style>
</head>
<body>

<div class="toggle-bar">
  <div class="toggle-inner">
    <button class="lang-btn" id="btn-es" onclick="setLang('es')">Español</button>
    <button class="lang-btn" id="btn-en" onclick="setLang('en')">English</button>
  </div>
</div>

<div class="wrap">

<!-- ================= SPANISH ================= -->
<div class="lang" id="lang-es">

<h1>Requisitos para compilar</h1>

<h2>1. Visual Studio</h2>
<ul>
<li><strong>Visual Studio 2022</strong> (Community es gratis y suficiente) — la versión más reciente está bien, no hace falta una "especial".</li>
<li>Al instalar, selecciona el workload <strong>"Desarrollo para el escritorio con C++"</strong> (<em>Desktop development with C++</em>). Esto te da el compilador MSVC, el linker y las herramientas de depuración.</li>
<li>Asegúrate de incluir el <strong>Windows 10/11 SDK</strong> más reciente (se instala junto con el workload de C++, pero revisa que esté marcado en el instalador — headers como <code>wincrypt.h</code>, <code>mfapi.h</code>, <code>audioclient.h</code>, <code>iphlpapi.h</code>, etc. vienen de ahí).</li>
</ul>

<h2>2. OpenVR SDK</h2>
<p>El proyecto incluye <code>#include &lt;openvr_driver.h&gt;</code>, que <strong>no viene con Visual Studio</strong>. Necesitas descargar el SDK de OpenVR de Valve:</p>
<ul>
<li>Repositorio: <a href="https://github.com/ValveSoftware/openvr" target="_blank">github.com/ValveSoftware/openvr</a></li>
<li>Necesitas la carpeta <code>headers/</code> (con <code>openvr_driver.h</code>) y el <code>.lib</code> correspondiente si tu build lo requiere (normalmente los drivers de SteamVR solo necesitan los headers, ya que se cargan dinámicamente vía <code>HmdDriverFactory</code>).</li>
<li>Agrega esa ruta en <strong>Project Properties → C/C++ → General → Additional Include Directories</strong>.</li>
</ul>

<h2>3. Configuración del proyecto</h2>
<ul>
<li><strong>Plataforma:</strong> x64 (SteamVR/OpenVR requiere 64 bits).</li>
<li><strong>Tipo de proyecto:</strong> DLL (<code>.dll</code>), ya que exporta <code>HmdDriverFactory</code> con <code>__declspec(dllexport)</code>.</li>
<li>Las librerías ya están enlazadas vía <code>#pragma comment(lib, ...)</code> en el propio código (<code>ws2_32.lib</code>, <code>crypt32.lib</code>, <code>mfplat.lib</code>, <code>mfuuid.lib</code>, <code>mf.lib</code>, <code>gdi32.lib</code>, <code>user32.lib</code>, <code>winmm.lib</code>, <code>avrt.lib</code>, <code>iphlpapi.lib</code>), así que no necesitas agregarlas manualmente al linker — solo que el SDK de Windows esté instalado.</li>
</ul>

<hr>

<h1>Configuración paso a paso en Visual Studio (rutas exactas)</h1>

<h2>1. Descargar y ubicar el SDK</h2>
<p>Descarga <strong>OpenVR SDK v2.12.14</strong> y descomprímelo en una ruta como:</p>
<pre><code>C:\Users\&lt;tu_usuario&gt;\tudireciondelarchivo\openvr-2.12.14\</code></pre>
<p>Deberías tener dentro las carpetas <code>headers</code>, <code>lib</code>, <code>bin</code>, <code>src</code>, etc.</p>

<h2>2. Directorios de inclusión (Include)</h2>
<p><strong>Propiedades del proyecto → C/C++ → General → Directorios de inclusión adicionales</strong></p>
<pre><code>C:\Users\&lt;tu_usuario&gt;\tudireciondelarchivo\openvr-2.12.14\headers;%(AdditionalIncludeDirectories)</code></pre>

<h2>3. Directorios de bibliotecas (Library)</h2>
<p><strong>Propiedades del proyecto → Vinculador → General → Directorios de bibliotecas adicionales</strong></p>
<pre><code>C:\Users\&lt;tu_usuario&gt;\tudireciondelarchivo\openvr-2.12.14\lib\win64;%(AdditionalLibraryDirectories)</code></pre>

<blockquote>En ambos casos, deja la <strong>Configuración</strong> en "Todas las config." y la <strong>Plataforma</strong> en "Todas las plataformas" para que aplique a Debug y Release por igual.</blockquote>

<h2>4. Propiedades generales</h2>
<table>
<tr><th>Propiedad</th><th>Valor</th></tr>
<tr><td>Directorio de salida</td><td><code>C:\Program Files (x86)\Steam\steamapps\common\SteamVR\drivers\MiDriverVR\bin\win64\</code></td></tr>
<tr><td>Nombre de destino</td><td><code>driver_MiDriverVR</code></td></tr>
<tr><td>Tipo de configuración</td><td>Biblioteca dinámica (.dll)</td></tr>
<tr><td>Versión del SDK de Windows</td><td>10.0 (última versión instalada)</td></tr>
<tr><td>Conjunto de herramientas de la plataforma</td><td>Visual Studio 2022 (v143)</td></tr>
<tr><td>Estándar de lenguaje C++</td><td>Estándar ISO C++17 (<code>/std:c++17</code>)</td></tr>
</table>
<blockquote>El <strong>Directorio de salida</strong> apunta directo a la carpeta de drivers de SteamVR, así el <code>.dll</code> compilado queda listo sin copiarlo manualmente.</blockquote>

<h2>5. Compilar</h2>
<ul>
<li>Selecciona plataforma <strong>x64</strong> (obligatorio).</li>
<li>Compila con <code>Ctrl + Shift + B</code> o <strong>Compilar → Compilar solución</strong>.</li>
<li>El resultado <code>driver_MiDriverVR.dll</code> aparecerá automáticamente en la carpeta de drivers de SteamVR.</li>
</ul>

<hr>

<h1>Instalación en SteamVR</h1>
<p>La estructura final dentro de <code>SteamVR\drivers\</code> debe verse así:</p>
<pre><code>SteamVR\drivers\MiDriverVR
├── bin\win64\driver_MiDriverVR.dll
├── resources
├── driver.vrdesc
└── driver.vrdrivermanifest</code></pre>
<ol>
<li>Copia (o verifica, según tu directorio de salida) los archivos <code>driver.vrdesc</code>, <code>driver.vrdrivermanifest</code> y la carpeta <code>resources</code> dentro de <code>SteamVR\drivers\MiDriverVR\</code>.</li>
<li>Abre SteamVR — el driver se carga automáticamente si el manifiesto está bien registrado.</li>
<li>Revisa <code>vrserver.txt</code> para confirmar que aparecen líneas con el prefijo <code>[CamVR]</code> sin errores.</li>
</ol>

<hr>

<h1>Puertos usados por el driver</h1>
<table>
<tr><th>Función</th><th>Puerto</th><th>Protocolo</th></tr>
<tr><td>Anuncio/discovery en red</td><td>47294</td><td>UDP (broadcast)</td></tr>
<tr><td>Streaming de video</td><td>47295</td><td>TCP</td></tr>
<tr><td>Cambio de calidad de video</td><td>47296</td><td>UDP</td></tr>
<tr><td>Flip de HMD (comando remoto)</td><td>47297</td><td>UDP</td></tr>
<tr><td>Streaming de audio</td><td>47298</td><td>TCP</td></tr>
</table>

<hr>

<h1>Notas técnicas</h1>
<ul>
<li>El encoder de video intenta usar <strong>hardware (Media Foundation MFT)</strong> primero; si no genera NALs en ~1.2s, cae automáticamente a <strong>software</strong>.</li>
<li>La captura detecta monitores virtuales automáticamente (no primarios) para evitar capturar el escritorio real.</li>
<li>La autenticación de sesión usa reto-respuesta con <strong>HMAC-SHA256</strong> truncado a 8 bytes, separando sesiones por loopback (PC) vs red (teléfono).</li>
<li>El suavizado de posición y curl de dedos usa un filtro exponencial simple para evitar saltos bruscos.</li>
</ul>

</div>
<!-- ================= /SPANISH ================= -->

<!-- ================= ENGLISH ================= -->
<div class="lang" id="lang-en">

<h1>Build requirements</h1>

<h2>1. Visual Studio</h2>
<ul>
<li><strong>Visual Studio 2022</strong> (Community is free and enough) — the latest version is fine, no "special" build needed.</li>
<li>During install, select the <strong>"Desktop development with C++"</strong> workload. This gives you the MSVC compiler, the linker, and the debugging tools.</li>
<li>Make sure the latest <strong>Windows 10/11 SDK</strong> is included (it installs alongside the C++ workload, but double-check it's checked in the installer — headers like <code>wincrypt.h</code>, <code>mfapi.h</code>, <code>audioclient.h</code>, <code>iphlpapi.h</code>, etc. come from there).</li>
</ul>

<h2>2. OpenVR SDK</h2>
<p>The project includes <code>#include &lt;openvr_driver.h&gt;</code>, which <strong>does not ship with Visual Studio</strong>. You need to download Valve's OpenVR SDK:</p>
<ul>
<li>Repository: <a href="https://github.com/ValveSoftware/openvr" target="_blank">github.com/ValveSoftware/openvr</a></li>
<li>You need the <code>headers/</code> folder (with <code>openvr_driver.h</code>) and the matching <code>.lib</code> if your build requires it (normally SteamVR drivers only need the headers, since they're loaded dynamically via <code>HmdDriverFactory</code>).</li>
<li>Add that path under <strong>Project Properties → C/C++ → General → Additional Include Directories</strong>.</li>
</ul>

<h2>3. Project configuration</h2>
<ul>
<li><strong>Platform:</strong> x64 (SteamVR/OpenVR requires 64-bit).</li>
<li><strong>Project type:</strong> DLL (<code>.dll</code>), since it exports <code>HmdDriverFactory</code> with <code>__declspec(dllexport)</code>.</li>
<li>The libraries are already linked via <code>#pragma comment(lib, ...)</code> in the code itself (<code>ws2_32.lib</code>, <code>crypt32.lib</code>, <code>mfplat.lib</code>, <code>mfuuid.lib</code>, <code>mf.lib</code>, <code>gdi32.lib</code>, <code>user32.lib</code>, <code>winmm.lib</code>, <code>avrt.lib</code>, <code>iphlpapi.lib</code>), so you don't need to add them manually to the linker — just make sure the Windows SDK is installed.</li>
</ul>

<hr>

<h1>Step-by-step setup in Visual Studio (exact paths)</h1>

<h2>1. Download and place the SDK</h2>
<p>Download <strong>OpenVR SDK v2.12.14</strong> and unzip it to a path like:</p>
<pre><code>C:\Users\&lt;your_user&gt;\path\to\file\openvr-2.12.14\</code></pre>
<p>Inside you should have the <code>headers</code>, <code>lib</code>, <code>bin</code>, <code>src</code> folders, etc.</p>

<h2>2. Include directories</h2>
<p><strong>Project Properties → C/C++ → General → Additional Include Directories</strong></p>
<pre><code>C:\Users\&lt;your_user&gt;\path\to\file\openvr-2.12.14\headers;%(AdditionalIncludeDirectories)</code></pre>

<h2>3. Library directories</h2>
<p><strong>Project Properties → Linker → General → Additional Library Directories</strong></p>
<pre><code>C:\Users\&lt;your_user&gt;\path\to\file\openvr-2.12.14\lib\win64;%(AdditionalLibraryDirectories)</code></pre>

<blockquote>In both cases, leave <strong>Configuration</strong> set to "All Configurations" and <strong>Platform</strong> set to "All Platforms" so it applies to Debug and Release alike.</blockquote>

<h2>4. General properties</h2>
<table>
<tr><th>Property</th><th>Value</th></tr>
<tr><td>Output Directory</td><td><code>C:\Program Files (x86)\Steam\steamapps\common\SteamVR\drivers\MiDriverVR\bin\win64\</code></td></tr>
<tr><td>Target Name</td><td><code>driver_MiDriverVR</code></td></tr>
<tr><td>Configuration Type</td><td>Dynamic Library (.dll)</td></tr>
<tr><td>Windows SDK Version</td><td>10.0 (latest installed)</td></tr>
<tr><td>Platform Toolset</td><td>Visual Studio 2022 (v143)</td></tr>
<tr><td>C++ Language Standard</td><td>ISO C++17 Standard (<code>/std:c++17</code>)</td></tr>
</table>
<blockquote>The <strong>Output Directory</strong> points directly at the SteamVR drivers folder, so the compiled <code>.dll</code> ends up ready without manually copying it.</blockquote>

<h2>5. Build</h2>
<ul>
<li>Select the <strong>x64</strong> platform (required).</li>
<li>Build with <code>Ctrl + Shift + B</code> or <strong>Build → Build Solution</strong>.</li>
<li>The resulting <code>driver_MiDriverVR.dll</code> will automatically appear in the SteamVR drivers folder.</li>
</ul>

<hr>

<h1>Installing in SteamVR</h1>
<p>The final structure inside <code>SteamVR\drivers\</code> should look like this:</p>
<pre><code>SteamVR\drivers\MiDriverVR
├── bin\win64\driver_MiDriverVR.dll
├── resources
├── driver.vrdesc
└── driver.vrdrivermanifest</code></pre>
<ol>
<li>Copy (or verify, depending on your output directory) the <code>driver.vrdesc</code>, <code>driver.vrdrivermanifest</code> files and the <code>resources</code> folder into <code>SteamVR\drivers\MiDriverVR\</code>.</li>
<li>Open SteamVR — the driver loads automatically if the manifest is registered correctly.</li>
<li>Check <code>vrserver.txt</code> to confirm lines with the <code>[CamVR]</code> prefix appear without errors.</li>
</ol>

<hr>

<h1>Ports used by the driver</h1>
<table>
<tr><th>Function</th><th>Port</th><th>Protocol</th></tr>
<tr><td>Network announce/discovery</td><td>47294</td><td>UDP (broadcast)</td></tr>
<tr><td>Video streaming</td><td>47295</td><td>TCP</td></tr>
<tr><td>Video quality change</td><td>47296</td><td>UDP</td></tr>
<tr><td>HMD flip (remote command)</td><td>47297</td><td>UDP</td></tr>
<tr><td>Audio streaming</td><td>47298</td><td>TCP</td></tr>
</table>

<hr>

<h1>Technical notes</h1>
<ul>
<li>The video encoder first tries to use <strong>hardware (Media Foundation MFT)</strong>; if it doesn't produce NALs within ~1.2s, it automatically falls back to <strong>software</strong>.</li>
<li>Capture automatically detects virtual (non-primary) monitors to avoid capturing the real desktop.</li>
<li>Session authentication uses a challenge-response scheme with <strong>HMAC-SHA256</strong> truncated to 8 bytes, keeping loopback (PC) and network (phone) sessions separate.</li>
<li>Position smoothing and finger curl use a simple exponential filter to avoid abrupt jumps.</li>
</ul>

</div>
<!-- ================= /ENGLISH ================= -->

</div>

<script>
function setLang(lang){
  document.getElementById('lang-es').classList.toggle('active', lang === 'es');
  document.getElementById('lang-en').classList.toggle('active', lang === 'en');
  document.getElementById('btn-es').classList.toggle('active', lang === 'es');
  document.getElementById('btn-en').classList.toggle('active', lang === 'en');
  document.documentElement.lang = lang;
  try { window.__lang = lang; } catch(e){}
}
setLang('es');
</script>

</body>
</html>
