
class PcmPlay extends AudioWorkletProcessor {
  constructor() {
    super();

    this.capacity = 16000;
    this.ring = new Float32Array(this.capacity);

    this.read = 0;
    this.write = 0;
    this.count = 0;
    this.playing = false;

    this.port.onmessage = (e) => {
      if (!(e.data instanceof ArrayBuffer)) return;

      const samples = new Int16Array(e.data);

      for (let i = 0; i < samples.length; i++) {
        if (this.count === this.capacity) {
          this.read = (this.read + 1) % this.capacity;
          this.count--;
        }

        this.ring[this.write] = samples[i] / 32768;
        this.write = (this.write + 1) % this.capacity;
        this.count++;
      }

      this.port.postMessage({
        type: "received",
        samples: samples.length,
        buffered: this.count
      });
    };
  }

  process(inputs, outputs) {
    const out = outputs[0][0];
    if (!out) return true;

    // Start after 80 ms instead of 200 ms.
    if (!this.playing && this.count >= 640) {
      this.playing = true;
    }

    for (let i = 0; i < out.length; i++) {
      if (this.playing && this.count > 0) {
        out[i] = this.ring[this.read];
        this.read = (this.read + 1) % this.capacity;
        this.count--;
      } else {
        out[i] = 0;
        this.playing = false;
      }
    }

    return true;
  }
}

registerProcessor("pcm-play", PcmPlay);
