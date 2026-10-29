// The sound card's side (gui/audio_stream.h's PlaybackGate, simplified): a
// ring of stereo frames from the emulator's Worker, which plays only once it
// holds the target and, after running dry, waits for it again; starting
// again with more than twice the target, it skips the oldest down to it.
// It tells the Worker how many frames it has taken in all (played, skipped
// or dropped) and whether it plays, which the emulator's servo follows
// (web/src/web_audio.h). The Worker's port comes in a message on this
// node's port.

const CAPACITY = 1 << 16;  // frames: 1.5 s at 44.1 kHz
const REPORT_FRAMES = 1024;

class Op1Output extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.target = options.processorOptions.targetFrames;
    this.ring = new Float32Array(CAPACITY * 2);
    this.read = 0;   // frames, in all
    this.write = 0;
    this.dropped = 0;  // frames a full ring could not hold, in all
    this.playing = false;
    this.sinceReport = 0;
    this.worker = null;
    this.port.onmessage = ({ data }) => {
      this.worker = data.port;
      this.worker.onmessage = ({ data: frames }) => this.push(frames);
    };
  }

  push(frames) {
    // A full ring drops what it cannot hold (the producer ran far ahead, or
    // the context was suspended): counted as taken, as the Worker counts
    // them written.
    const count = Math.min(frames.length / 2, CAPACITY - (this.write - this.read));
    this.dropped += frames.length / 2 - count;
    for (let i = 0; i < count; ++i) {
      const at = ((this.write + i) % CAPACITY) * 2;
      this.ring[at] = frames[2 * i];
      this.ring[at + 1] = frames[2 * i + 1];
    }
    this.write += count;
  }

  process(_inputs, outputs) {
    const [left, right] = outputs[0];
    const want = left.length;
    let available = this.write - this.read;
    if (!this.playing && available >= this.target) {
      this.playing = true;
      if (available > 2 * this.target) {
        this.read += available - this.target;
        available = this.target;
      }
    }
    let played = 0;
    if (this.playing) {
      played = Math.min(want, available);
      for (let i = 0; i < played; ++i) {
        const at = ((this.read + i) % CAPACITY) * 2;
        left[i] = this.ring[at];
        right[i] = this.ring[at + 1];
      }
      this.read += played;
      if (played < want) this.playing = false;
    }
    left.fill(0, played);
    right.fill(0, played);
    this.sinceReport += want;
    if (this.worker && this.sinceReport >= REPORT_FRAMES) {
      this.sinceReport = 0;
      this.worker.postMessage({ consumed: this.read + this.dropped, playing: this.playing });
    }
    return true;
  }
}

registerProcessor("op1-output", Op1Output);
