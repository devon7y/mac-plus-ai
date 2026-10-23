// Plays the Mac's sound: signed 8-bit mono samples at 22255 Hz (Mini vMac's Plus sound) from a
// SharedArrayBuffer ring that the emulator worker fills, resampled to the AudioContext's rate.
const AU_WRITE = 0, AU_READ = 1, AU_HEADER_BYTES = 16;

class MacAudio extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const { buffer, srcRate } = options.processorOptions;
    this.hdr = new Int32Array(buffer, 0, 4);
    this.data = new Int8Array(buffer, AU_HEADER_BYTES);
    this.size = this.data.length;
    this.step = srcRate / sampleRate;          // source samples per output sample
    this.target = Math.round(srcRate * 0.06);  // keep about 60 ms queued
    this.frac = 0;
    this.prev = 0;
    this.cur = 0;
    this.playing = false;
    this.skips = 0; this.blocks = 0;
  }

  process(inputs, outputs) {
    const out = outputs[0][0];
    const size = this.size;
    let r = Atomics.load(this.hdr, AU_READ);
    const w = Atomics.load(this.hdr, AU_WRITE);
    let avail = (w - r + size) % size;
    if (!this.playing) {
      if (avail < this.target) { out.fill(this.cur * 0.98); this.cur *= 0.98; return true; }
      this.playing = true;
    }
    if (avail > this.target * 2) {             // more than ~120 ms queued: drop the oldest, keep latency low
      r = (w - this.target + size) % size;
      avail = this.target;
      this.skips++;
    }
    if (++this.blocks % 750 === 0) this.port.postMessage({ skips: this.skips, avail });   // (about every 2 s)
    for (let i = 0; i < out.length; i++) {
      this.frac += this.step;
      while (this.frac >= 1) {
        this.frac -= 1;
        this.prev = this.cur;
        if (avail > 0) {
          this.cur = this.data[r] / 128;
          r = (r + 1) % size;
          avail--;
        } else {
          this.playing = false;                // ran dry: hold the last sample, refill first
        }
      }
      out[i] = this.prev + (this.cur - this.prev) * this.frac;
    }
    Atomics.store(this.hdr, AU_READ, r);
    return true;
  }
}

registerProcessor("mac-audio", MacAudio);
