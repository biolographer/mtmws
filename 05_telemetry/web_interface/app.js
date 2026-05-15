// =============================================================================
//  MTMWS Oscilloscope — host-side renderer
//  Receives COBS-framed binary telemetry from the Workshop Computer over
//  USB CDC (WebSerial) and renders a 4-channel rolling oscilloscope.
// =============================================================================

// --- Serial state ---
let port;
let reader;
let keepReading = true;

// --- Frame assembly buffer ---
// Bytes from the serial stream accumulate here until we hit a 0x00 delimiter.
const BUFFER_SIZE = 8192;
let rawBuffer = new Uint8Array(BUFFER_SIZE);
let bufferIndex = 0;

// --- Rolling trace storage ---
// Implemented as ring buffers with a single write index, so appending a sample
// is O(1) instead of O(n). The renderer reads them in logical order using
// `traceWriteIndex` as the "newest" pointer.
const TRACE_LENGTH = 1024;
const traceAudio1 = new Float32Array(TRACE_LENGTH);
const traceAudio2 = new Float32Array(TRACE_LENGTH);
const traceCV1    = new Float32Array(TRACE_LENGTH);
const traceCV2    = new Float32Array(TRACE_LENGTH);
let traceWriteIndex = 0; // points at next slot to write (oldest sample)

const TRACE_COLORS = {
    Audio1: '#00FF00',
    Audio2: '#00FFFF',
    CV1:    '#FF00FF',
    CV2:    '#FFFF00'
};

// --- Frame statistics (useful for debugging) ---
let framesReceived = 0;
let framesDropped  = 0;

// =============================================================================
//  WebSerial connection
// =============================================================================

async function connectToWorkshopComputer() {
    try {
        port = await navigator.serial.requestPort();
        // Baud rate is ignored by RP2040 CDC, but WebSerial requires a value.
        await port.open({ baudRate: 115200 });
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

// =============================================================================
//  COBS framing
// =============================================================================
//
// Wire format produced by the firmware:
//   <COBS-encoded payload> <0x00 delimiter>
//
// COBS encoding rule (matches firmware):
//   - The output begins with a "code" byte = 1 + (number of non-zero data
//     bytes before the first zero).
//   - Then those data bytes are copied literally.
//   - Each subsequent zero in the input is replaced by a new code byte
//     introducing the next run.
//   - Special case: if a run reaches 254 non-zeros without hitting a zero,
//     the code byte is 0xFF and NO implicit zero is appended after that run.
//
// Decoding rule:
//   - Read the first code byte. Copy (code - 1) data bytes literally.
//   - If code < 0xFF AND there is more data to come, append a single 0x00
//     to the output (this is the zero that was originally there).
//   - Repeat.
//
// The previous implementation had two bugs:
//   1. It emitted a phantom 0x00 at position 0 (treating the leading
//      overhead byte as if it represented a zero in the original data).
//   2. It guarded the trailing-zero emit with a "more data?" check that
//      let through the leading case but suppressed trailing legitimate runs
//      under some inputs.
// This version follows the canonical algorithm.

function cobsDecode(encoded, length) {
    // Allocate just enough — output is always strictly shorter than input.
    const decoded = new Uint8Array(length);
    let read_index  = 0;
    let write_index = 0;
    let first       = true;

    while (read_index < length) {
        const code = encoded[read_index++];
        if (code === 0) {
            // Should not appear inside a COBS-encoded packet.
            return null;
        }
        // Copy (code - 1) literal bytes.
        for (let i = 1; i < code; i++) {
            if (read_index >= length) return null; // malformed
            decoded[write_index++] = encoded[read_index++];
        }
        // Append the implicit zero that this run terminated, EXCEPT:
        //  - not after the very first overhead byte if it's at the start
        //    (handled by the "first" flag below — but actually the canonical
        //    rule is: append zero unless code == 0xFF (overflow run) OR
        //    we've consumed all input).
        if (code !== 0xFF && read_index < length) {
            decoded[write_index++] = 0;
        }
        first = false;
    }
    return decoded.subarray(0, write_index);
}

function processIncomingChunk(chunk) {
    for (let i = 0; i < chunk.length; i++) {
        const byte = chunk[i];
        if (byte === 0x00) {
            // End of frame. Decode and parse what we accumulated.
            if (bufferIndex > 0) {
                const decoded = cobsDecode(rawBuffer, bufferIndex);
                if (decoded) parseTelemetryFrame(decoded);
                else framesDropped++;
            }
            bufferIndex = 0;
        } else {
            if (bufferIndex < BUFFER_SIZE) {
                rawBuffer[bufferIndex++] = byte;
            } else {
                // Buffer overrun — abandon this frame and resync on next 0x00.
                bufferIndex = 0;
                framesDropped++;
            }
        }
    }
}

// =============================================================================
//  Frame parsing
// =============================================================================

function parseTelemetryFrame(decodedBytes) {
    // Expected size: 4 (header) + 4*64*2 (samples) + 6 (knobs) + 2 (sw+pad) = 524
    if (decodedBytes.length < 524) {
        framesDropped++;
        return;
    }

    // DataView needs an ArrayBuffer; use the view's underlying buffer plus offset.
    const view = new DataView(
        decodedBytes.buffer,
        decodedBytes.byteOffset,
        decodedBytes.byteLength
    );

    const syncWord = view.getUint16(0, true);
    if (syncWord !== 0xAA55) {
        framesDropped++;
        return;
    }

    const sampleCount = view.getUint16(2, true);
    if (sampleCount === 0 || sampleCount > 64) {
        framesDropped++;
        return;
    }

    // The four sample arrays come back-to-back in the struct, NOT interleaved.
    // Layout:  audio1[64] | audio2[64] | cv1[64] | cv2[64]
    const A1_BASE = 4;
    const A2_BASE = A1_BASE + 64 * 2;
    const C1_BASE = A2_BASE + 64 * 2;
    const C2_BASE = C1_BASE + 64 * 2;

    for (let i = 0; i < sampleCount; i++) {
        const off = i * 2;
        // Signed 12-bit, sign-extended into int16. Normalise to [-1, +1].
        const a1 = view.getInt16(A1_BASE + off, true) / 2048.0;
        const a2 = view.getInt16(A2_BASE + off, true) / 2048.0;
        const c1 = view.getInt16(C1_BASE + off, true) / 2048.0;
        const c2 = view.getInt16(C2_BASE + off, true) / 2048.0;

        traceAudio1[traceWriteIndex] = a1;
        traceAudio2[traceWriteIndex] = a2;
        traceCV1   [traceWriteIndex] = c1;
        traceCV2   [traceWriteIndex] = c2;
        traceWriteIndex = (traceWriteIndex + 1) % TRACE_LENGTH;
    }
    framesReceived++;
}

// =============================================================================
//  Rendering
// =============================================================================
//
// The trace arrays are circular: traceWriteIndex points at the slot that
// will be overwritten *next*, i.e. the oldest sample currently in memory.
// To render in time order we walk from there forward, wrapping around.

const canvas = document.getElementById('oscilloscope-display');
const ctx = canvas.getContext('2d');

function readLogical(arr, logicalIndex) {
    // logicalIndex 0 = oldest, TRACE_LENGTH-1 = newest
    return arr[(traceWriteIndex + logicalIndex) % TRACE_LENGTH];
}

function calculateTriggerIndex(arr, threshold = 0.0) {
    // Search the recent past (last ~200 logical samples) for a rising-edge
    // zero crossing, so the display locks even when the input is periodic.
    const SEARCH = 200;
    const newest = TRACE_LENGTH - 1;
    for (let i = newest - 1; i > newest - SEARCH; i--) {
        const a = readLogical(arr, i);
        const b = readLogical(arr, i + 1);
        if (a <= threshold && b > threshold) return i;
    }
    return 0;
}

function drawGraticule(context, w, h) {
    context.strokeStyle = '#222';
    context.lineWidth = 1;
    context.beginPath();
    for (let i = 0; i < w; i += 50) { context.moveTo(i, 0); context.lineTo(i, h); }
    for (let i = 0; i < h; i += 50) { context.moveTo(0, i); context.lineTo(w, i); }
    context.stroke();

    context.strokeStyle = '#444';
    context.beginPath();
    context.moveTo(0, h / 2);
    context.lineTo(w, h / 2);
    context.stroke();
}

function drawSignalTrace(context, arr, strokeColor, triggerStart) {
    context.beginPath();
    context.strokeStyle = strokeColor;
    context.lineWidth = 1.5;

    const visibleLength = TRACE_LENGTH - triggerStart;
    const xStep = canvas.width / (visibleLength > 0 ? visibleLength : TRACE_LENGTH);
    let xPos = 0;

    for (let i = triggerStart; i < TRACE_LENGTH; i++) {
        const v = readLogical(arr, i);
        const yPos = (canvas.height / 2) - (v * (canvas.height / 2));
        if (i === triggerStart) context.moveTo(xPos, yPos);
        else                    context.lineTo(xPos, yPos);
        xPos += xStep;
    }
    context.stroke();
}

function renderVisuals() {
    ctx.fillStyle = '#0a0a0a';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    drawGraticule(ctx, canvas.width, canvas.height);

    // Trigger off Audio1 so all four channels stay phase-locked.
    const triggerStart = calculateTriggerIndex(traceAudio1, 0.0);
    drawSignalTrace(ctx, traceAudio1, TRACE_COLORS.Audio1, triggerStart);
    drawSignalTrace(ctx, traceAudio2, TRACE_COLORS.Audio2, triggerStart);
    drawSignalTrace(ctx, traceCV1,    TRACE_COLORS.CV1,    triggerStart);
    drawSignalTrace(ctx, traceCV2,    TRACE_COLORS.CV2,    triggerStart);

    requestAnimationFrame(renderVisuals);
}

// Periodic stats logger — handy while debugging the link.
setInterval(() => {
    console.log(`frames OK=${framesReceived}  dropped=${framesDropped}`);
}, 2000);

requestAnimationFrame(renderVisuals);