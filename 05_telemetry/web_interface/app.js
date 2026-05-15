// --- Serial & Buffer Configuration ---
let port;
let reader;
let keepReading = true;

const BUFFER_SIZE = 8192; 
let rawBuffer = new Uint8Array(BUFFER_SIZE);
let bufferIndex = 0;

// Application state arrays (Rolling Timeline)
const TRACE_LENGTH = 1024;
const traceAudio1 = new Float32Array(TRACE_LENGTH);
const traceAudio2 = new Float32Array(TRACE_LENGTH);
const traceCV1    = new Float32Array(TRACE_LENGTH);
const traceCV2    = new Float32Array(TRACE_LENGTH);

const TRACE_COLORS = {
    Audio1: '#00FF00', // Bright Green
    Audio2: '#00FFFF', // Cyan
    CV1:    '#FF00FF', // Magenta
    CV2:    '#FFFF00'  // Yellow
};

// --- Web Serial API Logic ---

async function connectToWorkshopComputer() {
    try {
        port = await navigator.serial.requestPort();
        await port.open({ baudRate: 115200 }); // Baud rate ignored by RP2040 CDC
        document.getElementById('connection-status').innerText = "Connected: Streaming Data...";
        document.getElementById('connection-status').style.color = "#00FF00";
        keepReading = true;
        readLoop(); 
    } catch (error) {
        console.error("Hardware connection failed:", error);
        document.getElementById('connection-status').innerText = "Connection Failed";
        document.getElementById('connection-status').style.color = "#FF0000";
    }
}

async function readLoop() {
    while (port.readable && keepReading) {
        reader = port.readable.getReader();
        try {
            while (true) {
                const { value, done } = await reader.read();
                if (done) break; 
                if (value) processIncomingChunk(value);
            }
        } catch (error) {
            console.error("Stream disrupted:", error);
        } finally {
            reader.releaseLock();
        }
    }
}

// --- COBS Decoding Logic ---

function cobsDecode(encoded) {
    const decoded = new Uint8Array(encoded.length);
    let read_index = 0;
    let write_index = 0;
    
    while (read_index < encoded.length) {
        let code = encoded[read_index++];
        for (let i = 1; i < code; i++) {
            decoded[write_index++] = encoded[read_index++];
        }
        if (code < 0xFF && read_index < encoded.length) {
            decoded[write_index++] = 0;
        }
    }
    return decoded.slice(0, write_index);
}

function processIncomingChunk(chunk) {
    for (let i = 0; i < chunk.length; i++) {
        const byte = chunk[i];
        if (byte === 0x00) {
            if (bufferIndex > 0) {
                const encodedFrame = rawBuffer.slice(0, bufferIndex);
                const decodedFrame = cobsDecode(encodedFrame);
                parseTelemetryFrame(decodedFrame);
            }
            bufferIndex = 0; 
        } else {
            if (bufferIndex < BUFFER_SIZE) {
                rawBuffer[bufferIndex++] = byte;
            } else {
                bufferIndex = 0; // Discard overgrown buffer
            }
        }
    }
}

// --- Data Parsing ---

function parseTelemetryFrame(decodedBytes) {
    // 524 is the expected byte size of TelemetryFrame without struct padding issues.
    // Ensure this matches your specific C++ compiler's struct size output.
    if (decodedBytes.length < 520) return; 

    const view = new DataView(decodedBytes.buffer);
    const syncWord = view.getUint16(0, true);
    if (syncWord !== 0xAA55) return; 

    const sampleCount = view.getUint16(2, true);
    let offset = 4;
    
    for (let i = 0; i < sampleCount; i++) {
        // Normalize 12-bit ADC (0-4095) to -1.0 to 1.0 float space
        const a1 = (view.getUint16(offset, true) / 2047.5) - 1.0; offset += 2;
        const a2 = (view.getUint16(offset, true) / 2047.5) - 1.0; offset += 2;
        const c1 = (view.getUint16(offset, true) / 2047.5) - 1.0; offset += 2;
        const c2 = (view.getUint16(offset, true) / 2047.5) - 1.0; offset += 2;
        
        pushToRollingBuffer(traceAudio1, a1);
        pushToRollingBuffer(traceAudio2, a2);
        pushToRollingBuffer(traceCV1, c1);
        pushToRollingBuffer(traceCV2, c2);
    }
}

function pushToRollingBuffer(array, newValue) {
    array.copyWithin(0, 1);
    array[array.length - 1] = newValue;
}

// --- Canvas Rendering Loop ---

const canvas = document.getElementById('oscilloscope-display');
const ctx = canvas.getContext('2d');
let visualizationRunning = true;

function calculateTriggerIndex(dataArray, triggerThreshold = 0.0) {
    // Look backward through recent data for a rising edge zero-crossing
    for (let i = dataArray.length - 2; i > dataArray.length - 200; i--) {
        if (dataArray[i] <= triggerThreshold && dataArray[i+1] > triggerThreshold) {
            return i;
        }
    }
    return 0; // Default to start if no trigger found
}

function drawGraticule(context, w, h) {
    context.strokeStyle = '#222';
    context.lineWidth = 1;
    context.beginPath();
    // Horizontal and vertical grid lines
    for(let i = 0; i < w; i += 50) { context.moveTo(i, 0); context.lineTo(i, h); }
    for(let i = 0; i < h; i += 50) { context.moveTo(0, i); context.lineTo(w, i); }
    context.stroke();
    
    // Center Axis
    context.strokeStyle = '#444';
    context.beginPath();
    context.moveTo(0, h/2); context.lineTo(w, h/2);
    context.stroke();
}

function drawSignalTrace(context, dataArray, strokeColor) {
    context.beginPath();
    context.strokeStyle = strokeColor;
    context.lineWidth = 1.5;
    
    // Use Audio1 as the master trigger source for stability
    const triggerStart = calculateTriggerIndex(traceAudio1, 0.0);
    const visibleLength = dataArray.length - triggerStart;
    const xStep = canvas.width / (visibleLength > 0 ? visibleLength : dataArray.length);
    
    let xPos = 0;
    
    for (let i = triggerStart; i < dataArray.length; i++) {
        const floatValue = dataArray[i]; 
        const yPos = (canvas.height / 2) - (floatValue * (canvas.height / 2));
        
        if (i === triggerStart) {
            context.moveTo(xPos, yPos);
        } else {
            context.lineTo(xPos, yPos);
        }
        xPos += xStep;
    }
    context.stroke();
}

function renderVisuals() {
    if (!visualizationRunning) return;
    
    ctx.fillStyle = '#0a0a0a'; 
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    
    drawGraticule(ctx, canvas.width, canvas.height);
    
    drawSignalTrace(ctx, traceAudio1, TRACE_COLORS.Audio1);
    drawSignalTrace(ctx, traceAudio2, TRACE_COLORS.Audio2);
    drawSignalTrace(ctx, traceCV1,    TRACE_COLORS.CV1);
    drawSignalTrace(ctx, traceCV2,    TRACE_COLORS.CV2);
    
    requestAnimationFrame(renderVisuals);
}

// Kick off renderer
requestAnimationFrame(renderVisuals);