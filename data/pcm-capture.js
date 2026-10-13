// Capture 8-kHz mono PCM in Codec2 frame-sized blocks.
class PcmCapture extends AudioWorkletProcessor {
  constructor(){super();this.buf=new Int16Array(160);this.pos=0;this.enabled=false;
    this.port.onmessage=e=>{if(e.data==='ON')this.enabled=true;else if(e.data==='OFF'){this.enabled=false;this.pos=0;}};
  }
  process(inputs){
    const ch=inputs[0]&&inputs[0][0];if(!ch)return true;
    if(!this.enabled)return true;
    for(let i=0;i<ch.length;i++){
      const v=Math.max(-1,Math.min(1,ch[i]));
      this.buf[this.pos++]=v<0?Math.round(v*32768):Math.round(v*32767);
      if(this.pos===160){this.port.postMessage(this.buf.buffer.slice(0));this.pos=0;}
    }
    return true;
  }
}
registerProcessor('pcm-capture',PcmCapture);
