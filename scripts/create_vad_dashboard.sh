#!/usr/bin/env node

/**
 * MachAudio Interactive Spectrogram & VAD Analyzer Generator
 * Packages raw PCM audio and VAD probability arrays into a self-contained, 
 * interactive HTML dashboard featuring high-fidelity browser-side STFT, 
 * Web Audio API playback, and dual synced visualizer lanes.
 */

const fs = require('fs');
const path = require('path');

const ANSI_RESET = "\x1b[0m";
const ANSI_GREEN = "\x1b[32m";
const ANSI_RED = "\x1b[31m";
const ANSI_YELLOW = "\x1b[33m";
const ANSI_CYAN = "\x1b[36m";

function logInfo(msg) {
    console.log(`${ANSI_GREEN}[INFO]${ANSI_RESET} ${msg}`);
}

function logError(msg) {
    console.error(`${ANSI_RED}[ERROR]${ANSI_RESET} ${msg}`);
}

function printUsage() {
    console.error(`${ANSI_CYAN}MachAudio Interactive Spectrogram Generator${ANSI_RESET}`);
    console.error("Usage: ./create_vad_spectrum.sh <audio_pcm_file> <vad_probabilities_file> <output_html_file>");
    console.error("\nArguments:");
    console.error("  audio_pcm_file          Raw 16-bit signed PCM (mono, 16kHz) audio file");
    console.error("  vad_probabilities_file  VAD probability file (space/newline delimited text OR binary 32-bit floats)");
    console.error("  output_html_file        Path to save the self-contained interactive HTML analyzer (.html)");
    process.exit(1);
}

// 1. Argument Validation
const args = process.argv.slice(2);
if (args.length < 3) {
    printUsage();
}

const audioPath = path.resolve(args[0]);
const vadPath = path.resolve(args[1]);
let outputPath = path.resolve(args[2]);

// Auto-correct extension to .html if needed
if (path.extname(outputPath) === '.svg') {
    outputPath = outputPath.slice(0, -4) + '.html';
}

if (!fs.existsSync(audioPath)) {
    logError(`Audio input file not found: ${audioPath}`);
    process.exit(1);
}

if (!fs.existsSync(vadPath)) {
    logError(`VAD probabilities file not found: ${vadPath}`);
    process.exit(1);
}

// 2. Read and Parse Audio PCM (Int16) to validate
logInfo(`Loading PCM audio from: ${audioPath}`);
const audioBuf = fs.readFileSync(audioPath);
const sampleCount = Math.floor(audioBuf.length / 2);

if (sampleCount === 0) {
    logError("Audio file is empty.");
    process.exit(1);
}

// 3. Read and Parse VAD file
logInfo(`Loading VAD probabilities from: ${vadPath}`);
const vadBuf = fs.readFileSync(vadPath);

if (vadBuf.length === 0) {
    logError("VAD probabilities file is empty.");
    process.exit(1);
}

// Heuristics for ASCII text
function isTextFile(buffer) {
    const limit = Math.min(buffer.length, 1024);
    for (let i = 0; i < limit; i++) {
        const charCode = buffer[i];
        if (charCode < 32 && charCode !== 9 && charCode !== 10 && charCode !== 13) {
            return false;
        }
    }
    return true;
}

let vadConf = [];
let useFloatLE = true;
if (isTextFile(vadBuf)) {
    const text = vadBuf.toString('utf8');
    vadConf = text.trim().split(/\s+/).map(parseFloat).filter(n => !isNaN(n));
} else {
    const count = Math.floor(vadBuf.length / 4);
    const confLE = new Float32Array(count);
    const confBE = new Float32Array(count);
    let outOfRangeLE = 0;
    let outOfRangeBE = 0;
    
    for (let i = 0; i < count; i++) {
        const valLE = vadBuf.readFloatLE(i * 4);
        const valBE = vadBuf.readFloatBE(i * 4);
        confLE[i] = valLE;
        confBE[i] = valBE;
        
        if (valLE < -1.05 || valLE > 1.05) outOfRangeLE++;
        if (valBE < -1.05 || valBE > 1.05) outOfRangeBE++;
    }
    useFloatLE = outOfRangeLE <= outOfRangeBE;
    vadConf = useFloatLE ? confLE : confBE;
}

logInfo(`Verified audio (${sampleCount} samples) and VAD (${vadConf.length} segments).`);

// 4. Base64 Encode Raw Streams for Inline HTML embedding
logInfo("Packaging binary streams into Base64 envelopes...");
const audioBase64 = audioBuf.toString('base64');

// Ensure VAD is normalized as Float32 little-endian array buffer
const vadFloatArray = new Float32Array(vadConf);
const vadBase64 = Buffer.from(vadFloatArray.buffer).toString('base64');

// 5. Generate Breathtaking Interactive HTML Spectrogram Dashboard
logInfo("Building interactive HTML dashboard...");

const htmlContent = `<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>MachAudio Interactive Spectrogram & VAD Analyzer</title>
    <link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600;800&family=JetBrains+Mono:wght@300;400&display=swap" rel="stylesheet">
    <style>
        :root {
            --bg-base: #0a0a0c;
            --bg-surface: #121216;
            --bg-element: #1a1a24;
            --border-glow: rgba(0, 255, 204, 0.15);
            --accent-primary: #00ffcc;
            --accent-secondary: #ff007f;
            --accent-green: #00ff88;
            --text-main: #f1f5f9;
            --text-muted: #94a3b8;
            --font-main: 'Outfit', sans-serif;
            --font-mono: 'JetBrains Mono', monospace;
        }

        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }

        body {
            background-color: var(--bg-base);
            color: var(--text-main);
            font-family: var(--font-main);
            height: 100vh;
            display: flex;
            flex-direction: column;
            overflow: hidden;
            padding: 16px;
        }

        header {
            display: flex;
            align-items: center;
            justify-content: space-between;
            margin-bottom: 16px;
            padding: 12px 24px;
            background: var(--bg-surface);
            border: 1px solid var(--border-glow);
            border-radius: 16px;
            box-shadow: 0 8px 32px 0 rgba(0, 0, 0, 0.4);
            backdrop-filter: blur(10px);
            flex-shrink: 0; /* Lock header size */
        }

        .logo-section h1 {
            font-size: 24px;
            font-weight: 800;
            background: linear-gradient(135deg, var(--accent-primary), #00a3ff);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            letter-spacing: 0.5px;
            display: flex;
            align-items: center;
            gap: 10px;
        }

        .logo-section h1 span {
            font-size: 12px;
            font-weight: 400;
            color: var(--text-muted);
            border: 1px solid var(--text-muted);
            padding: 2px 6px;
            border-radius: 6px;
            vertical-align: middle;
        }

        .meta-stats {
            display: flex;
            gap: 20px;
            font-family: var(--font-mono);
            font-size: 13px;
            color: var(--text-muted);
        }

        .stat-item strong {
            color: var(--accent-primary);
        }

        .dashboard-container {
            display: grid;
            grid-template-columns: 280px 1fr;
            gap: 16px;
            flex-grow: 1;
            min-height: 0; /* CRITICAL for inner-grid scrolling */
            min-width: 0;
            width: 100%;
        }

        /* Sidebar Controls */
        .controls-card {
            background: var(--bg-surface);
            border: 1px solid rgba(255, 255, 255, 0.05);
            border-radius: 16px;
            padding: 20px;
            display: flex;
            flex-direction: column;
            gap: 20px;
            box-shadow: 0 8px 32px 0 rgba(0, 0, 0, 0.3);
            height: 100%;
            overflow-y: auto; /* Local controls scrollbar if needed */
        }

        .section-title {
            font-size: 16px;
            font-weight: 600;
            text-transform: uppercase;
            letter-spacing: 1px;
            color: var(--text-muted);
            border-bottom: 1px solid rgba(255, 255, 255, 0.05);
            padding-bottom: 8px;
        }

        .control-group {
            display: flex;
            flex-direction: column;
            gap: 8px;
        }

        .control-group label {
            font-size: 14px;
            font-weight: 600;
            color: var(--text-muted);
            display: flex;
            justify-content: space-between;
        }

        .control-group label span {
            font-family: var(--font-mono);
            color: var(--accent-primary);
        }

        input[type="range"] {
            -webkit-appearance: none;
            width: 100%;
            height: 6px;
            background: var(--bg-element);
            border-radius: 3px;
            outline: none;
        }

        input[type="range"]::-webkit-slider-thumb {
            -webkit-appearance: none;
            width: 16px;
            height: 16px;
            border-radius: 50%;
            background: var(--accent-primary);
            cursor: pointer;
            box-shadow: 0 0 10px var(--accent-primary);
            transition: transform 0.1s;
        }

        input[type="range"]::-webkit-slider-thumb:hover {
            transform: scale(1.2);
        }

        select {
            width: 100%;
            padding: 10px 14px;
            background: var(--bg-element);
            border: 1px solid rgba(255, 255, 255, 0.08);
            border-radius: 8px;
            color: var(--text-main);
            font-family: var(--font-main);
            outline: none;
            cursor: pointer;
        }

        .btn {
            background: linear-gradient(135deg, var(--accent-primary), #00a3ff);
            border: none;
            padding: 12px 20px;
            border-radius: 8px;
            color: #050508;
            font-weight: 600;
            font-size: 15px;
            cursor: pointer;
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 10px;
            transition: all 0.2s;
            box-shadow: 0 4px 15px rgba(0, 255, 204, 0.2);
        }

        .btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 20px rgba(0, 255, 204, 0.35);
        }

        .btn.btn-secondary {
            background: var(--bg-element);
            border: 1px solid rgba(255, 255, 255, 0.1);
            color: var(--text-main);
            box-shadow: none;
        }

        .btn.btn-secondary:hover {
            border-color: var(--accent-primary);
            box-shadow: 0 0 10px rgba(0, 255, 204, 0.1);
        }

        /* Visualization Area */
        .visualization-pane {
            display: flex;
            flex-direction: column;
            gap: 16px;
            height: 100%;
            min-height: 0;
            min-width: 0;
            overflow: hidden;
        }

        .viewport-card {
            background: var(--bg-surface);
            border: 1px solid rgba(255, 255, 255, 0.03);
            border-radius: 16px;
            padding: 16px 20px 20px 20px;
            box-shadow: 0 8px 32px 0 rgba(0, 0, 0, 0.3);
            position: relative;
            display: flex;
            flex-direction: column;
            flex: 1;
            min-height: 0;
            min-width: 0;
            width: 100%;
            overflow: hidden;
        }

        .canvas-container {
            position: relative;
            width: 100%;
            cursor: crosshair;
            overflow-x: auto; /* Show scrollbar only if overflowing */
            overflow-y: hidden;
            border-radius: 8px;
            border: 1px solid rgba(255, 255, 255, 0.05);
            background: #000000;
            flex-grow: 1;
            min-height: 120px;
            scrollbar-width: auto;
            scrollbar-color: rgba(0, 255, 204, 0.5) rgba(255, 255, 255, 0.03);
        }

        .canvas-container::-webkit-scrollbar {
            height: 12px;
            display: block;
        }

        .canvas-container::-webkit-scrollbar-track {
            background: rgba(255, 255, 255, 0.03);
            border-radius: 6px;
        }

        .canvas-container::-webkit-scrollbar-thumb {
            background: rgba(0, 255, 204, 0.5);
            border-radius: 6px;
            border: 2px solid var(--bg-surface);
            box-shadow: 0 0 8px rgba(0, 255, 204, 0.4);
        }

        .canvas-container::-webkit-scrollbar-thumb:hover {
            background: rgba(0, 255, 204, 0.8);
            box-shadow: 0 0 12px rgba(0, 255, 204, 0.7);
        }

        canvas {
            display: block;
        }

        /* Glassmorphic Loading Spinner Overlay */
        .loading-overlay {
            position: absolute;
            top: 0;
            left: 0;
            right: 0;
            bottom: 0;
            background: rgba(10, 10, 14, 0.75);
            backdrop-filter: blur(8px);
            display: flex;
            align-items: center;
            justify-content: center;
            z-index: 100;
            opacity: 0;
            pointer-events: none;
            transition: opacity 0.3s ease;
            border-radius: 16px;
        }

        .loading-overlay.active {
            opacity: 1;
            pointer-events: auto;
        }

        .spinner {
            width: 48px;
            height: 48px;
            border: 4px solid rgba(0, 255, 204, 0.1);
            border-top: 4px solid var(--accent-primary);
            border-radius: 50%;
            animation: spin 1s linear infinite;
            box-shadow: 0 0 15px rgba(0, 255, 204, 0.2);
        }

        @keyframes spin {
            0% { transform: rotate(0deg); }
            100% { transform: rotate(360deg); }
        }

        /* Playback & Marker line overlays */
        .playhead-marker {
            position: absolute;
            top: 0;
            bottom: 0;
            width: 2px;
            background-color: var(--accent-secondary);
            pointer-events: none;
            box-shadow: 0 0 10px var(--accent-secondary);
            z-index: 10;
            display: none;
        }

        .hover-marker {
            position: absolute;
            top: 0;
            bottom: 0;
            width: 1px;
            background-color: rgba(255, 255, 255, 0.25);
            pointer-events: none;
            z-index: 9;
            display: none;
        }

        .card-header-with-tabs {
            display: flex;
            align-items: center;
            justify-content: space-between;
            margin-bottom: 12px;
            flex-shrink: 0;
        }

        .card-title {
            font-size: 16px;
            font-weight: 600;
            color: var(--text-main);
            display: flex;
            align-items: center;
            gap: 10px;
        }

        .card-title span {
            width: 10px;
            height: 10px;
            border-radius: 50%;
            display: inline-block;
        }

        .indicator-spectrogram { background-color: var(--accent-primary); box-shadow: 0 0 8px var(--accent-primary); }
        .indicator-waveform { background-color: var(--accent-secondary); box-shadow: 0 0 8px var(--accent-secondary); }

        /* Tooltip style */
        .inspector-tooltip {
            position: absolute;
            background: rgba(10, 10, 14, 0.95);
            border: 1px solid var(--accent-primary);
            border-radius: 8px;
            padding: 10px 14px;
            font-family: var(--font-mono);
            font-size: 12px;
            color: var(--text-main);
            pointer-events: none;
            z-index: 100;
            box-shadow: 0 4px 20px rgba(0,0,0,0.5);
            backdrop-filter: blur(5px);
            display: none;
        }

        .tooltip-row {
            display: flex;
            justify-content: space-between;
            gap: 20px;
            margin-bottom: 4px;
        }

        .tooltip-row:last-child {
            margin-bottom: 0;
        }

        .tooltip-label {
            color: var(--text-muted);
        }

        .tooltip-value {
            font-weight: 600;
            text-align: right;
        }

        .value-active-speech {
            color: var(--accent-green);
        }

        .value-silence {
            color: var(--text-muted);
        }
    </style>
</head>
<body>

    <header>
        <div class="logo-section">
            <h1>MachAudio <span>VAD Spectrogram Analyzer</span></h1>
        </div>
        <div class="meta-stats">
            <div class="stat-item">Samples: <strong id="lbl-samples">0</strong></div>
            <div class="stat-item">Rate: <strong>16,000 Hz</strong></div>
            <div class="stat-item">Duration: <strong id="lbl-duration">0.0s</strong></div>
            <div class="stat-item">VAD Slots: <strong id="lbl-vad-slots">0</strong></div>
        </div>
    </header>

    <div class="dashboard-container">
        <!-- Sidebar Controls Panel -->
        <div class="controls-card">
            <div class="control-group">
                <div class="section-title">Playback</div>
                <button class="btn" id="btn-play-pause">
                    <svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor">
                        <path d="M8 5v14l11-7z"/>
                    </svg>
                    Play Audio
                </button>
            </div>

            <div class="control-group">
                <div class="section-title">Time Scale Zoom</div>
                <label>Zoom Level <span id="val-zoom">2x</span></label>
                <input type="range" id="slider-zoom" min="0" max="10" step="1" value="2">
            </div>

            <div class="control-group">
                <div class="section-title">VAD Filters</div>
                <label>VAD Threshold <span id="val-threshold">0.50</span></label>
                <input type="range" id="slider-threshold" min="0.0" max="1.0" step="0.01" value="0.50">
            </div>

            <div class="control-group">
                <div class="section-title">Spectrogram Specs</div>
                <label>FFT Size <span id="val-fft">512</span></label>
                <select id="select-fft">
                    <option value="256">256 bins (Finer Time)</option>
                    <option value="512" selected>512 bins (Balanced)</option>
                    <option value="1024">1024 bins (Finer Freq)</option>
                </select>
            </div>

            <div class="control-group">
                <label>Color Palette</label>
                <select id="select-palette">
                    <option value="magma" selected>Plasma Inferno (Classic)</option>
                    <option value="matrix">Matrix Neon (Green/Cyan)</option>
                    <option value="cyan">Deep Cyber (Ice Blue)</option>
                    <option value="grayscale">High Contrast Grayscale</option>
                </select>
            </div>
            
            <div style="flex-grow: 1;"></div>
            
            <div class="control-group">
                <button class="btn btn-secondary" onclick="window.location.reload()">Reset View</button>
            </div>
        </div>

        <!-- Main Spectrogram Viewport Panels -->
        <div class="visualization-pane">
            
            <!-- Lane 1: The Synced Spectrogram -->
            <div class="viewport-card" id="card-spectrogram" style="flex: 1.6; min-height: 0;">
                <div class="loading-overlay" id="loading-spectrogram">
                    <div class="spinner"></div>
                </div>
                <div class="card-header-with-tabs">
                    <div class="card-title">
                        <span class="indicator-spectrogram"></span>
                        High-Definition Fourier Spectrogram (Frequency vs. Time)
                    </div>
                </div>
                <div class="canvas-container" id="container-spectrogram" style="position: relative;">
                    <div id="spacer-spectrogram" style="height: 1px;"></div>
                    <canvas id="canvas-spectrogram" style="position: absolute; left: 0; top: 0; z-index: 1; pointer-events: none;"></canvas>
                    <canvas id="canvas-spectrogram-overlay" style="position: absolute; left: 0; top: 0; z-index: 2; pointer-events: none;"></canvas>
                    <div class="playhead-marker" id="playhead-spectrogram" style="z-index: 3;"></div>
                    <div class="hover-marker" id="hover-spectrogram" style="z-index: 3;"></div>
                </div>
            </div>

            <!-- Lane 2: Waveform Envelope & VAD Lane Overlay -->
            <div class="viewport-card" id="card-waveform" style="flex: 1.0; min-height: 0;">
                <div class="card-header-with-tabs">
                    <div class="card-title">
                        <span class="indicator-waveform"></span>
                        Waveform Peak Envelope & Interactive VAD Overlays
                    </div>
                </div>
                <div class="canvas-container" id="container-waveform" style="position: relative;">
                    <div id="spacer-waveform" style="height: 1px;"></div>
                    <canvas id="canvas-waveform" style="position: absolute; left: 0; top: 0; z-index: 1; pointer-events: none;"></canvas>
                    <div class="playhead-marker" id="playhead-waveform" style="z-index: 2;"></div>
                    <div class="hover-marker" id="hover-waveform" style="z-index: 2;"></div>
                </div>
            </div>

        </div>
    </div>

    <!-- Inspector floating Tooltip -->
    <div class="inspector-tooltip" id="tooltip"></div>

    <script>
        // Inline encoded binary assets from server packaging
        const PCM_BASE64 = "${audioBase64}";
        const VAD_BASE64 = "${vadBase64}";
        const SAMPLE_RATE = 16000;
        const PTIME_MS = 20;
        const SAMPLES_PER_PTIME = (SAMPLE_RATE / 1000) * PTIME_MS; // 320 samples

        // 1. Unpack base64 streams
        function base64ToBuffer(b64) {
            const binary = atob(b64);
            const bytes = new Uint8Array(binary.length);
            for (let i = 0; i < binary.length; i++) {
                bytes[i] = binary.charCodeAt(i);
            }
            return bytes.buffer;
        }

        log("Decompressing binary streams...");
        const pcmArray = new Int16Array(base64ToBuffer(PCM_BASE64));
        const vadArray = new Float32Array(base64ToBuffer(VAD_BASE64));
        log("Successfully loaded PCM & VAD streams.");

        // Update top metrics
        const totalDuration = pcmArray.length / SAMPLE_RATE;
        document.getElementById('lbl-samples').innerText = pcmArray.length.toLocaleString();
        document.getElementById('lbl-duration').innerText = totalDuration.toFixed(2) + 's';
        document.getElementById('lbl-vad-slots').innerText = vadArray.length.toLocaleString();

        function log(msg) {
            console.log("[Analyzer] " + msg);
        }

        // --- WebGL Spectrogram Hardware-Accelerated Renderer ---
        class WebGLSpectrogram {
            constructor(canvas) {
                this.canvas = canvas;
                this.gl = canvas.getContext('webgl') || canvas.getContext('experimental-webgl');
                if (!this.gl) {
                    console.warn("WebGL not supported, falling back to 2D canvas.");
                    return;
                }
                this.initShaders();
                this.initBuffers();
                this.spectrogramTex = null;
                this.paletteTex = null;
            }

            initShaders() {
                const gl = this.gl;
                const vsSource = 'attribute vec2 a_position; varying vec2 v_texCoord; void main() { v_texCoord = vec2((a_position.x + 1.0) / 2.0, (a_position.y + 1.0) / 2.0); gl_Position = vec4(a_position, 0.0, 1.0); }';
                const fsSource = 'precision mediump float; varying vec2 v_texCoord; uniform sampler2D u_spectrogram; uniform sampler2D u_palette; uniform float u_startS; uniform float u_endS; void main() { float s = mix(u_startS, u_endS, v_texCoord.x); float val = texture2D(u_spectrogram, vec2(s, v_texCoord.y)).r; vec4 color = texture2D(u_palette, vec2(val, 0.5)); gl_FragColor = color; }';

                const vs = gl.createShader(gl.VERTEX_SHADER);
                gl.shaderSource(vs, vsSource);
                gl.compileShader(vs);
                if (!gl.getShaderParameter(vs, gl.COMPILE_STATUS)) {
                    console.error("Vertex shader compilation failed:", gl.getShaderInfoLog(vs));
                }

                const fs = gl.createShader(gl.FRAGMENT_SHADER);
                gl.shaderSource(fs, fsSource);
                gl.compileShader(fs);
                if (!gl.getShaderParameter(fs, gl.COMPILE_STATUS)) {
                    console.error("Fragment shader compilation failed:", gl.getShaderInfoLog(fs));
                }

                this.program = gl.createProgram();
                gl.attachShader(this.program, vs);
                gl.attachShader(this.program, fs);
                gl.linkProgram(this.program);
                if (!gl.getProgramParameter(this.program, gl.LINK_STATUS)) {
                    console.error("Shader program linking failed:", gl.getProgramInfoLog(this.program));
                }

                this.positionLoc = gl.getAttribLocation(this.program, 'a_position');
                this.spectrogramLoc = gl.getUniformLocation(this.program, 'u_spectrogram');
                this.paletteLoc = gl.getUniformLocation(this.program, 'u_palette');
                this.startSLoc = gl.getUniformLocation(this.program, 'u_startS');
                this.endSLoc = gl.getUniformLocation(this.program, 'u_endS');
            }

            initBuffers() {
                const gl = this.gl;
                this.buffer = gl.createBuffer();
                gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
                const vertices = new Float32Array([
                    -1, -1,
                     1, -1,
                    -1,  1,
                    -1,  1,
                     1, -1,
                     1,  1
                ]);
                gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.STATIC_DRAW);
            }

            setSpectrogramData(data, width, height) {
                const gl = this.gl;
                if (!gl) return;

                if (this.spectrogramTex) {
                    gl.deleteTexture(this.spectrogramTex);
                }

                this.spectrogramTex = gl.createTexture();
                gl.activeTexture(gl.TEXTURE0);
                gl.bindTexture(gl.TEXTURE_2D, this.spectrogramTex);
                
                // CRITICAL: Set pixel store alignment to 1 for arbitrary width textures
                gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
                
                gl.texImage2D(
                    gl.TEXTURE_2D, 0, gl.LUMINANCE, width, height, 0,
                    gl.LUMINANCE, gl.UNSIGNED_BYTE, data
                );

                gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
                gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
                gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
                gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
            }

            setPalette(paletteFunc) {
                const gl = this.gl;
                if (!gl) return;

                if (this.paletteTex) {
                    gl.deleteTexture(this.paletteTex);
                }

                const paletteData = new Uint8Array(256 * 3);
                for (let i = 0; i < 256; i++) {
                    const rgb = paletteFunc(i / 255);
                    paletteData[i * 3] = rgb[0];
                    paletteData[i * 3 + 1] = rgb[1];
                    paletteData[i * 3 + 2] = rgb[2];
                }

                this.paletteTex = gl.createTexture();
                gl.activeTexture(gl.TEXTURE1);
                gl.bindTexture(gl.TEXTURE_2D, this.paletteTex);
                
                // Set pixel store alignment to 1 for safety
                gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
                
                gl.texImage2D(
                    gl.TEXTURE_2D, 0, gl.RGB, 256, 1, 0,
                    gl.RGB, gl.UNSIGNED_BYTE, paletteData
                );

                gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
                gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
                gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
                gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
            }

            render(scrollLeft, displayWidth, virtualWidth) {
                const gl = this.gl;
                if (!gl || !this.spectrogramTex || !this.paletteTex) return;

                gl.viewport(0, 0, gl.canvas.width, gl.canvas.height);
                gl.clearColor(0, 0, 0, 1);
                gl.clear(gl.COLOR_BUFFER_BIT);

                gl.useProgram(this.program);

                gl.enableVertexAttribArray(this.positionLoc);
                gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
                gl.vertexAttribPointer(this.positionLoc, 2, gl.FLOAT, false, 0, 0);

                // Set textures
                gl.activeTexture(gl.TEXTURE0);
                gl.bindTexture(gl.TEXTURE_2D, this.spectrogramTex);
                gl.uniform1i(this.spectrogramLoc, 0);

                gl.activeTexture(gl.TEXTURE1);
                gl.bindTexture(gl.TEXTURE_2D, this.paletteTex);
                gl.uniform1i(this.paletteLoc, 1);

                // Calculate texture range coordinates
                const startS = scrollLeft / virtualWidth;
                const endS = (scrollLeft + displayWidth) / virtualWidth;

                gl.uniform1f(this.startSLoc, startS);
                gl.uniform1f(this.endSLoc, endS);

                gl.drawArrays(gl.TRIANGLES, 0, 6);
            }
        }

        // --- Web Worker Background Thread Code String ---
        const workerCode = 'class FastFFT { constructor(size) { this.size = size; this.twiddleRe = new Float32Array(size / 2); this.twiddleIm = new Float32Array(size / 2); for (let i = 0; i < size / 2; i++) { const angle = -2 * Math.PI * i / size; this.twiddleRe[i] = Math.cos(angle); this.twiddleIm[i] = Math.sin(angle); } this.revTable = new Int32Array(size); let limit = 1; let bit = size >> 1; while (limit < size) { for (let i = 0; i < limit; i++) { this.revTable[i + limit] = this.revTable[i] + bit; } limit <<= 1; bit >>= 1; } } forward(re, im) { const n = this.size; for (let i = 0; i < n; i++) { const j = this.revTable[i]; if (i < j) { let temp = re[i]; re[i] = re[j]; re[j] = temp; temp = im[i]; im[i] = im[j]; im[j] = temp; } } for (let step = 2; step <= n; step <<= 1) { const halfStep = step >> 1; const tabStep = n / step; for (let i = 0; i < n; i += step) { for (let j = 0; j < halfStep; j++) { const k = i + j; const l = k + halfStep; const tIdx = j * tabStep; const wr = this.twiddleRe[tIdx]; const wi = this.twiddleIm[tIdx]; const tRe = re[l] * wr - im[l] * wi; const tIm = re[l] * wi + im[l] * wr; re[l] = re[k] - tRe; im[l] = im[k] - tIm; re[k] += tRe; im[k] += tIm; } } } } } self.onmessage = function(e) { const { pcm, fftSize, samplesPerPtime } = e.data; const pcmArray = new Int16Array(pcm); const stepSize = samplesPerPtime; const numFrames = Math.floor((pcmArray.length - fftSize) / stepSize); const binCount = fftSize / 2; const win = new Float32Array(fftSize); for (let i = 0; i < fftSize; i++) { win[i] = 0.5 * (1 - Math.cos(2 * Math.PI * i / (fftSize - 1))); } const fftEngine = new FastFFT(fftSize); const re = new Float32Array(fftSize); const im = new Float32Array(fftSize); const output = new Uint8Array(numFrames * binCount); const minDb = -85; const maxDb = -20; for (let f = 0; f < numFrames; f++) { const offset = f * stepSize; for (let i = 0; i < fftSize; i++) { re[i] = pcmArray[offset + i] * win[i]; im[i] = 0.0; } fftEngine.forward(re, im); const outOffset = f * binCount; for (let i = 0; i < binCount; i++) { const r = re[i]; const valIm = im[i]; const mag = Math.sqrt(r * r + valIm * valIm); const db = mag > 0.00001 ? 20 * Math.log10(mag) : -120; const norm = Math.min(255, Math.max(0, ((db - minDb) / (maxDb - minDb)) * 255)); output[outOffset + i] = Math.round(norm); } } self.postMessage({ output: output.buffer, numFrames }, [output.buffer]); };';

        // --- Color Palettes (Returns [R, G, B] arrays for direct performance rendering) ---
        const palettes = {
            magma: (norm) => {
                if (norm < 0.1) return [Math.round(norm * 10 * 20), 5, Math.round(norm * 10 * 30)];
                if (norm < 0.4) {
                    const t = (norm - 0.1) / 0.3;
                    return [Math.round(20 + t * 120), 5, Math.round(30 + t * 100)];
                }
                if (norm < 0.8) {
                    const t = (norm - 0.4) / 0.4;
                    return [Math.round(140 + t * 115), Math.round(5 + t * 120), Math.round(130 - t * 80)];
                }
                const t = (norm - 0.8) / 0.2;
                return [255, Math.round(125 + t * 130), Math.round(50 + t * 205)];
            },
            matrix: (norm) => {
                if (norm < 0.2) return [0, Math.round(norm * 5 * 60), Math.round(norm * 5 * 20)];
                if (norm < 0.7) {
                    const t = (norm - 0.2) / 0.5;
                    return [0, Math.round(60 + t * 195), Math.round(20 + t * 180)];
                }
                const t = (norm - 0.7) / 0.3;
                return [Math.round(t * 220), 255, Math.round(200 + t * 55)];
            },
            cyan: (norm) => {
                const b = Math.round(norm * 255);
                const g = Math.round(Math.pow(norm, 1.5) * 230);
                const r = Math.round(Math.pow(norm, 2.5) * 100);
                return [r, g, b];
            },
            grayscale: (norm) => {
                const val = Math.round(norm * 255);
                return [val, val, val];
            }
        };

        // --- Application State ---
        let fftSize = 512;
        let vadThreshold = 0.50;
        let activePalette = 'magma';
        const ZOOM_FACTORS = [1, 1.5, 2, 3, 4, 6, 8, 12, 16, 24, 32];
        let zoomFactor = 2; // Default zoom factor relative to viewport
        let zoomLevel = 3;  // Pixels per STFT frame (calculated dynamically)

        function recalculateZoom() {
            if (spectrogramMag.length === 0) return;
            const viewportWidth = containerSpectrogram.clientWidth || 800;
            zoomLevel = Math.max(0.001, (viewportWidth * zoomFactor) / spectrogramMag.length);
        }
        
        let audioCtx = null;
        let audioSource = null;
        let audioBuffer = null;
        let isPlaying = false;
        let startTime = 0;
        let pauseTime = 0;
        let animationFrameId = null;

        // UI Selectors
        const containerSpectrogram = document.getElementById('container-spectrogram');
        const containerWaveform = document.getElementById('container-waveform');
        const canvasSpectrogram = document.getElementById('canvas-spectrogram');
        const canvasSpectrogramOverlay = document.getElementById('canvas-spectrogram-overlay');
        const canvasWaveform = document.getElementById('canvas-waveform');
        const sliderThreshold = document.getElementById('slider-threshold');
        const valThreshold = document.getElementById('val-threshold');
        const sliderZoom = document.getElementById('slider-zoom');
        const valZoom = document.getElementById('val-zoom');
        const selectFft = document.getElementById('select-fft');
        const valFft = document.getElementById('val-fft');
        const selectPalette = document.getElementById('select-palette');
        const btnPlayPause = document.getElementById('btn-play-pause');

        // Synced Spectrogram arrays
        let spectrogramMag = []; // 2D grid: [time_index][freq_index]
        let webglRenderer = null;
        let stftWorker = null;

        function initWorker() {
            if (stftWorker) return;
            const blob = new Blob([workerCode], { type: 'application/javascript' });
            const url = URL.createObjectURL(blob);
            stftWorker = new Worker(url);

            stftWorker.onmessage = function(e) {
                const { output, numFrames } = e.data;
                const magData = new Uint8Array(output);

                spectrogramMag = { length: numFrames };

                if (webglRenderer) {
                    webglRenderer.setSpectrogramData(magData, numFrames, fftSize / 2);
                    webglRenderer.setPalette(palettes[activePalette]);
                }

                document.getElementById('loading-spectrogram').classList.remove('active');

                recalculateZoom();
                updateVisuals();
            };
        }

        // --- STFT Engine ---
        function runSTFT() {
            log("Posting STFT job to background worker with FFT size: " + fftSize);
            initWorker();
            document.getElementById('loading-spectrogram').classList.add('active');
            
            stftWorker.postMessage({
                pcm: pcmArray.buffer,
                fftSize: fftSize,
                samplesPerPtime: SAMPLES_PER_PTIME
            });
        }

        // --- Update scrollbar spacer sizes to fit timeline width ---
        function updateSpacerWidths() {
            const virtualWidth = spectrogramMag.length * zoomLevel;
            document.getElementById('spacer-spectrogram').style.width = virtualWidth + 'px';
            document.getElementById('spacer-waveform').style.width = virtualWidth + 'px';
        }

        // --- Render Spectrogram to Canvas (Fixed Viewport virtualized rendering) ---
        function renderSpectrogram() {
            if (spectrogramMag.length === 0) return;

            const displayWidth = containerSpectrogram.clientWidth;
            const displayHeight = containerSpectrogram.clientHeight || 280;
            const scrollLeft = containerSpectrogram.scrollLeft;

            canvasSpectrogram.style.left = scrollLeft + 'px';
            canvasSpectrogramOverlay.style.left = scrollLeft + 'px';

            const dpr = window.devicePixelRatio || 1;

            // Resize WebGL canvas
            canvasSpectrogram.width = displayWidth * dpr;
            canvasSpectrogram.height = displayHeight * dpr;
            canvasSpectrogram.style.width = displayWidth + 'px';
            canvasSpectrogram.style.height = displayHeight + 'px';

            // Resize Overlay 2D canvas
            canvasSpectrogramOverlay.width = displayWidth * dpr;
            canvasSpectrogramOverlay.height = displayHeight * dpr;
            canvasSpectrogramOverlay.style.width = displayWidth + 'px';
            canvasSpectrogramOverlay.style.height = displayHeight + 'px';

            // Hardware Accelerated WebGL Render
            const virtualWidth = spectrogramMag.length * zoomLevel;
            if (webglRenderer) {
                webglRenderer.render(scrollLeft, displayWidth, virtualWidth);
            }

            // Draw glowing VAD curve on 2D Overlay canvas
            const ctx = canvasSpectrogramOverlay.getContext('2d');
            ctx.clearRect(0, 0, displayWidth, displayHeight);
            ctx.scale(dpr, dpr);

            ctx.lineWidth = 2.5;
            ctx.shadowBlur = 8;
            ctx.shadowColor = '#ff007f';
            ctx.strokeStyle = '#ff007f';
            ctx.beginPath();
            
            const frameCount = spectrogramMag.length;
            const virtualTotalWidth = frameCount * zoomLevel;
            const vadStepX = virtualTotalWidth / vadArray.length;
            const startVad = Math.max(0, Math.floor(scrollLeft / vadStepX) - 2);
            const endVad = Math.min(vadArray.length, Math.ceil((scrollLeft + displayWidth) / vadStepX) + 2);

            for (let i = startVad; i < endVad; i++) {
                const rawProb = vadArray[i];
                const active = rawProb >= vadThreshold;
                const prob = active ? (rawProb < 0 ? 0.0 : Math.min(1.0, Math.max(0.0, rawProb))) : 0.0;

                const x = i * vadStepX + (vadStepX / 2) - scrollLeft;
                const y = displayHeight - (prob * (displayHeight - 20)) - 10;
                
                if (i === startVad) ctx.moveTo(x, y);
                else ctx.lineTo(x, y);
            }
            ctx.stroke();
            ctx.shadowBlur = 0;
        }

        // --- Render Synced Waveform Envelope & VAD highlight Overlay (Fixed Viewport virtualized rendering) ---
        function renderWaveform() {
            const ctx = canvasWaveform.getContext('2d');
            const dpr = window.devicePixelRatio || 1;
            
            const displayWidth = containerWaveform.clientWidth;
            const displayHeight = containerWaveform.clientHeight || 160;
            
            canvasWaveform.width = displayWidth * dpr;
            canvasWaveform.height = displayHeight * dpr;
            ctx.scale(dpr, dpr);
            
            canvasWaveform.style.width = displayWidth + 'px';
            canvasWaveform.style.height = displayHeight + 'px';
            
            const scrollLeft = containerWaveform.scrollLeft;
            canvasWaveform.style.left = scrollLeft + 'px';
            
            ctx.fillStyle = '#0b0b0d';
            ctx.fillRect(0, 0, displayWidth, displayHeight);

            const virtualTotalWidth = spectrogramMag.length * zoomLevel;

            // 1. Draw VAD Highlights behind waveform (only visible range)
            if (vadArray.length > 0) {
                const xStep = virtualTotalWidth / vadArray.length;
                const startVad = Math.max(0, Math.floor(scrollLeft / xStep) - 2);
                const endVad = Math.min(vadArray.length, Math.ceil((scrollLeft + displayWidth) / xStep) + 2);

                // Draw Full Height highlight overlay
                for (let i = startVad; i < endVad; i++) {
                    const rawProb = vadArray[i];
                    const isSpeech = rawProb >= vadThreshold;
                    
                    if (isSpeech && rawProb > 0.0) {
                        const x = i * xStep - scrollLeft;
                        ctx.fillStyle = 'rgba(0, 255, 136, 0.20)'; // Glowing solid green overlay
                        ctx.fillRect(x, 0, xStep + 0.5, displayHeight);
                    }
                }

                // Draw solid VAD Bottom status bar
                const bottomBarHeight = 12;
                ctx.fillStyle = '#101014';
                ctx.fillRect(0, displayHeight - bottomBarHeight, displayWidth, bottomBarHeight);

                for (let i = startVad; i < endVad; i++) {
                    const rawProb = vadArray[i];
                    const isSpeech = rawProb >= vadThreshold;
                    
                    if (isSpeech && rawProb > 0.0) {
                        const x = i * xStep - scrollLeft;
                        ctx.fillStyle = 'rgb(0, 255, 136)'; // Solid premium VAD active bar
                        ctx.fillRect(x, displayHeight - bottomBarHeight, xStep + 0.5, bottomBarHeight);
                    }
                }
            }

            // 2. Draw Waveform Peaks (only visible pixel columns)
            ctx.lineWidth = 1;
            ctx.strokeStyle = '#00ffcc';
            ctx.beginPath();
            
            const samplesPerPixel = pcmArray.length / virtualTotalWidth;
            const centerY = (displayHeight - 12) / 2; // Exclude bottom bar
            const maxAmp = 32768.0;

            const startPx = Math.max(0, Math.floor(scrollLeft) - 2);
            const endPx = Math.min(virtualTotalWidth, Math.ceil(scrollLeft + displayWidth) + 2);

            for (let x = startPx; x < endPx; x++) {
                const start = Math.floor(x * samplesPerPixel);
                let end = Math.floor((x + 1) * samplesPerPixel);
                if (end > pcmArray.length) end = pcmArray.length;

                let peak = 0;
                for (let s = start; s < end; s++) {
                    if (Math.abs(pcmArray[s]) > Math.abs(peak)) {
                        peak = pcmArray[s];
                    }
                }

                const offset = (Math.abs(peak) / maxAmp) * centerY;
                const drawX = x - scrollLeft;
                ctx.moveTo(drawX, centerY - offset);
                ctx.lineTo(drawX, centerY + offset);
            }
            ctx.stroke();
        }

        function updateMarkerPositions() {
            const progress = isPlaying 
                ? (audioCtx ? (audioCtx.currentTime - startTime) / totalDuration : 0)
                : (pauseTime / totalDuration);
            
            const displayWidth = spectrogramMag.length * zoomLevel;
            const xPos = progress * displayWidth;
            
            const markerSpec = document.getElementById('playhead-spectrogram');
            const markerWave = document.getElementById('playhead-waveform');

            if (progress > 0 && progress < 1.0) {
                markerSpec.style.display = 'block';
                markerWave.style.display = 'block';
                markerSpec.style.left = xPos + 'px';
                markerWave.style.left = xPos + 'px';
            } else {
                if (pauseTime > 0) {
                    markerSpec.style.display = 'block';
                    markerWave.style.display = 'block';
                    markerSpec.style.left = xPos + 'px';
                    markerWave.style.left = xPos + 'px';
                } else {
                    markerSpec.style.display = 'none';
                    markerWave.style.display = 'none';
                }
            }
        }

        // --- Recalculate and Redraw visualizer views ---
        function updateVisuals() {
            updateSpacerWidths();
            renderSpectrogram();
            renderWaveform();
            updateMarkerPositions();
        }

        // --- Playback Engine (Web Audio API) ---
        function initAudio() {
            if (audioCtx) return;
            audioCtx = new (window.AudioContext || window.webkitAudioContext)();
            
            // Build AudioBuffer from raw Int16 samples
            audioBuffer = audioCtx.createBuffer(1, pcmArray.length, SAMPLE_RATE);
            const channelData = audioBuffer.getChannelData(0);
            for (let i = 0; i < pcmArray.length; i++) {
                channelData[i] = pcmArray[i] / 32768.0;
            }
        }

        function togglePlayback() {
            initAudio();
            if (audioCtx.state === 'suspended') {
                audioCtx.resume();
            }

            if (isPlaying) {
                // Pause
                isPlaying = false;
                pauseTime = audioCtx.currentTime - startTime;
                if (audioSource) {
                    audioSource.stop();
                    audioSource.disconnect();
                    audioSource = null;
                }
                btnPlayPause.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor"><path d="M8 5v14l11-7z"/></svg> Resume Audio';
                cancelAnimationFrame(animationFrameId);
            } else {
                // Play
                isPlaying = true;
                if (pauseTime >= totalDuration) {
                    pauseTime = 0; // Loop wrap-around
                }
                startTime = audioCtx.currentTime - pauseTime;
                
                audioSource = audioCtx.createBufferSource();
                audioSource.buffer = audioBuffer;
                audioSource.connect(audioCtx.destination);
                audioSource.start(0, pauseTime);
                
                audioSource.onended = () => {
                    if (isPlaying) {
                        isPlaying = false;
                        pauseTime = 0;
                        btnPlayPause.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor"><path d="M8 5v14l11-7z"/></svg> Play Audio';
                        cancelAnimationFrame(animationFrameId);
                        hidePlayheads();
                    }
                };

                btnPlayPause.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor"><path d="M6 19h4V5H6v14zm8-14v14h4V5h-4z"/></svg> Pause Audio';
                
                updatePlayheadPosition();
            }
        }

        function hidePlayheads() {
            document.getElementById('playhead-spectrogram').style.display = 'none';
            document.getElementById('playhead-waveform').style.display = 'none';
        }

        function updatePlayheadPosition() {
            if (!isPlaying) return;
            
            const curTime = audioCtx.currentTime - startTime;
            const progress = curTime / totalDuration;
            
            if (progress >= 1.0) {
                hidePlayheads();
                return;
            }

            const displayWidth = spectrogramMag.length * zoomLevel;
            const xPos = progress * displayWidth;
            
            const markerSpec = document.getElementById('playhead-spectrogram');
            const markerWave = document.getElementById('playhead-waveform');

            markerSpec.style.display = 'block';
            markerWave.style.display = 'block';

            // Set inline absolute coordinates for exact lining
            markerSpec.style.left = xPos + 'px';
            markerWave.style.left = xPos + 'px';

            // Auto-scroll to center the playhead smoothly
            const viewportWidth = containerSpectrogram.clientWidth;
            const scrollTarget = xPos - viewportWidth / 2;
            
            isSyncingScroll = true;
            containerSpectrogram.scrollLeft = scrollTarget;
            containerWaveform.scrollLeft = scrollTarget;
            isSyncingScroll = false;

            // Trigger redraw of visible virtual chunks
            updateVisuals();

            animationFrameId = requestAnimationFrame(updatePlayheadPosition);
        }

        function seekTo(progress) {
            const wasPlaying = isPlaying;
            if (isPlaying) {
                togglePlayback(); // Pause
            }
            pauseTime = progress * totalDuration;
            
            const displayWidth = spectrogramMag.length * zoomLevel;
            const xPos = progress * displayWidth;

            // Move markers directly in pixels
            const markerSpec = document.getElementById('playhead-spectrogram');
            const markerWave = document.getElementById('playhead-waveform');
            markerSpec.style.display = 'block';
            markerSpec.style.left = xPos + 'px';
            markerWave.style.display = 'block';
            markerWave.style.left = xPos + 'px';

            // Scroll viewports to center seek target
            const viewportWidth = containerSpectrogram.clientWidth;
            
            isSyncingScroll = true;
            containerSpectrogram.scrollLeft = xPos - viewportWidth / 2;
            containerWaveform.scrollLeft = xPos - viewportWidth / 2;
            isSyncingScroll = false;

            if (wasPlaying) {
                togglePlayback(); // Resume
            } else {
                updateVisuals();
            }
        }

        // --- Seek & Navigation handlers ---
        function handleCanvasClick(e, container) {
            const rect = container.getBoundingClientRect();
            const x = e.clientX - rect.left + container.scrollLeft;
            const displayWidth = spectrogramMag.length * zoomLevel;
            const progress = x / displayWidth;
            seekTo(Math.min(1.0, Math.max(0.0, progress)));
        }

        // --- Inspector Tooltip Hover Logic ---
        function handleMouseMove(e, container, type) {
            const rect = container.getBoundingClientRect();
            const x = e.clientX - rect.left + container.scrollLeft;
            const y = e.clientY - rect.top;
            
            const displayWidth = spectrogramMag.length * zoomLevel;
            const progress = x / displayWidth;
            const timestamp = progress * totalDuration;

            // Calculate precise VAD segment probability
            const vadIdx = Math.floor(progress * vadArray.length);
            const vadProb = vadIdx >= 0 && vadIdx < vadArray.length ? vadArray[vadIdx] : -1.0;
            
            const hoverSpec = document.getElementById('hover-spectrogram');
            const hoverWave = document.getElementById('hover-waveform');
            const tooltip = document.getElementById('tooltip');

            // Draw synced hover guidelines in pixels
            hoverSpec.style.display = 'block';
            hoverSpec.style.left = x + 'px';
            hoverWave.style.display = 'block';
            hoverWave.style.left = x + 'px';

            // Show Tooltip
            tooltip.style.display = 'block';
            
            // Center tooltip based on cursor
            tooltip.style.left = (e.clientX + 16) + 'px';
            tooltip.style.top = (e.clientY + 16) + 'px';

            let frequencyHtml = '';
            if (type === 'spectrogram') {
                // Origin is bottom, frequencies are inverse to vertical client position
                const freqNorm = 1.0 - (y / container.clientHeight);
                const freqHz = Math.round(freqNorm * (SAMPLE_RATE / 2));
                frequencyHtml = '<div class="tooltip-row"><span class="tooltip-label">Frequency:</span><span class="tooltip-value">' + freqHz.toLocaleString() + ' Hz</span></div>';
            }

            const speechStatus = vadProb >= vadThreshold 
                ? '<span class="value-active-speech">Active Speech (' + Math.round(vadProb * 100) + '%)</span>'
                : (vadProb < 0.0 ? '<span class="value-silence">Disabled</span>' : '<span class="value-silence">Silence (' + Math.round(vadProb * 100) + '%)</span>');

            tooltip.innerHTML = 
                '<div class="tooltip-row"><span class="tooltip-label">Time:</span><span class="tooltip-value">' + timestamp.toFixed(3) + 's</span></div>' +
                frequencyHtml +
                '<div class="tooltip-row"><span class="tooltip-label">VAD State:</span><span class="tooltip-value">' + speechStatus + '</span></div>';
        }

        function handleMouseLeave() {
            document.getElementById('hover-spectrogram').style.display = 'none';
            document.getElementById('hover-waveform').style.display = 'none';
            document.getElementById('tooltip').style.display = 'none';
        }

        // --- Synced Scroll Event Listeners ---
        let isSyncingScroll = false;
        containerSpectrogram.addEventListener('scroll', () => {
            if (!isSyncingScroll) {
                isSyncingScroll = true;
                containerWaveform.scrollLeft = containerSpectrogram.scrollLeft;
                isSyncingScroll = false;
            }
            updateVisuals(); // Redraw newly scrolled virtual columns
        });

        containerWaveform.addEventListener('scroll', () => {
            if (!isSyncingScroll) {
                isSyncingScroll = true;
                containerSpectrogram.scrollLeft = containerWaveform.scrollLeft;
                isSyncingScroll = false;
            }
            updateVisuals(); // Redraw newly scrolled virtual columns
        });

        function setZoomFactor(newFactor) {
            if (spectrogramMag.length === 0) return;
            const viewportWidth = containerSpectrogram.clientWidth || 800;
            const oldVirtualWidth = spectrogramMag.length * zoomLevel;
            const centerProgress = (containerSpectrogram.scrollLeft + viewportWidth / 2) / oldVirtualWidth;

            zoomFactor = newFactor;
            recalculateZoom();

            const newVirtualWidth = spectrogramMag.length * zoomLevel;
            const newScrollLeft = centerProgress * newVirtualWidth - viewportWidth / 2;

            // Sync scroll positions without triggering full rendering loop recursively
            isSyncingScroll = true;
            containerSpectrogram.scrollLeft = newScrollLeft;
            containerWaveform.scrollLeft = newScrollLeft;
            isSyncingScroll = false;

            updateVisuals();
        }

        // --- Event Listeners Setup ---
        window.addEventListener('resize', () => {
            recalculateZoom();
            updateVisuals();
        });

        sliderThreshold.addEventListener('input', (e) => {
            vadThreshold = parseFloat(e.target.value);
            valThreshold.innerText = vadThreshold.toFixed(2);
            updateVisuals();
        });

        sliderZoom.addEventListener('input', (e) => {
            const index = parseInt(e.target.value);
            const factor = ZOOM_FACTORS[index];
            valZoom.innerText = factor + 'x';
            setZoomFactor(factor);
        });

        selectFft.addEventListener('change', (e) => {
            fftSize = parseInt(e.target.value);
            valFft.innerText = fftSize;
            runSTFT();
            recalculateZoom();
            updateVisuals();
        });

        selectPalette.addEventListener('change', (e) => {
            activePalette = e.target.value;
            if (webglRenderer) {
                webglRenderer.setPalette(palettes[activePalette]);
            }
            renderSpectrogram();
        });

        btnPlayPause.addEventListener('click', togglePlayback);

        // Synced Click to Seek & Hover inspect mappings
        [canvasSpectrogram, canvasWaveform].forEach(c => {
            const parent = c.parentElement;
            const type = c.id === 'canvas-spectrogram' ? 'spectrogram' : 'waveform';
            
            parent.addEventListener('click', (e) => handleCanvasClick(e, parent));
            parent.addEventListener('mousemove', (e) => handleMouseMove(e, parent, type));
            parent.addEventListener('mouseleave', handleMouseLeave);
        });

        // Initialize WebGL and Background Worker
        webglRenderer = new WebGLSpectrogram(canvasSpectrogram);
        initWorker();

        // Initialize viewports
        runSTFT();
    </script>
</body>
</html>
`;

try {
    fs.writeFileSync(outputPath, htmlContent, 'utf8');
    logInfo(`Successfully generated interactive Spectrogram & VAD dashboard: ${outputPath}`);
    logInfo(`Open this dashboard in any modern web browser to interactively analyze, listen, and filter the VAD signals!`);
} catch (err) {
    logError(`Failed to write HTML dashboard output: ${err.message}`);
    process.exit(1);
}
