// node world/gamebox_ktx2.cjs DIR in.ktx2 out.rgba: a KTX2 (Basis) picture's first level as raw RGBA,
// by Basis Universal's own transcoder as three.js ships it (DIR has basis_transcoder.js and .wasm).
// Prints the width and height.
const fs = require('fs'), path = require('path'), vm = require('vm');
const [dir, input, output] = process.argv.slice(2);
const ctx = { console, WebAssembly, TextDecoder, setTimeout, clearTimeout, performance };
vm.runInNewContext(fs.readFileSync(path.join(dir, 'basis_transcoder.js'), 'utf8') + '\n;this.BASIS = BASIS;', ctx);
ctx.BASIS({ wasmBinary: fs.readFileSync(path.join(dir, 'basis_transcoder.wasm')) }).then(B => {
  B.initializeBasis();
  const f = new B.KTX2File(new Uint8Array(fs.readFileSync(input)));
  if (!f.isValid() || !f.startTranscoding()) throw new Error(input + ' is no KTX2 the transcoder takes');
  const RGBA32 = 13;
  const out = new Uint8Array(f.getImageTranscodedSizeInBytes(0, 0, 0, RGBA32));
  if (!f.transcodeImage(out, 0, 0, 0, RGBA32, 0, -1, -1)) throw new Error('transcoding ' + input + ' failed');
  fs.writeFileSync(output, out);
  console.log(f.getWidth(), f.getHeight());
});
