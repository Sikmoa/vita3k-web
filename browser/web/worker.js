// M1 worker shell. The generated Emscripten module is loaded in this worker.
const post = (message) => self.postMessage(message);

try {
  const moduleUrl = new URL('./vita3k_web.js', self.location.href).href;
  const { default: createModule } = await import(moduleUrl);
  const module = await createModule({
    locateFile: (file) => new URL(`./${file}`, self.location.href).href,
    print: (message) => post({ type: 'log', message }),
    printErr: (message) => post({ type: 'log', message }),
  });
  post({ type: 'ready' });
  self.onmessage = ({ data }) => {
    if (data?.type === 'shutdown' && module._vita3k_web_shutdown) {
      module._vita3k_web_shutdown();
      self.close();
    }
  };
} catch (error) {
  post({ type: 'error', message: String(error) });
}
