// run_wasi.mjs — run a wasm32-wasi program under Node's WASI runtime.
// Used to execute wasm_host_demo.wasm (wasmp compiled to WebAssembly), which
// generates .wasm modules from inside the sandbox into a preopened /out dir.
//
//   node --experimental-wasi-unstable-preview1 run_wasi.mjs <program.wasm> <outDir>
import { WASI } from 'node:wasi';
import { readFileSync, mkdirSync } from 'node:fs';
import { argv } from 'node:process';

const wasmPath = argv[2] ?? 'wasm_host_demo.wasm';
const outDir = argv[3] ?? 'out';
mkdirSync(outDir, { recursive: true });

const wasi = new WASI({
  version: 'preview1',
  args: [wasmPath],
  env: {},
  preopens: { '/out': outDir },
});

const bytes = readFileSync(wasmPath);
const mod = await WebAssembly.compile(bytes);
const instance = await WebAssembly.instantiate(mod, wasi.getImportObject());
wasi.start(instance);
