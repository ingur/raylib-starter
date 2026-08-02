// /save is an IDBFS mount, synced before main() runs so saves exist at boot
// autoPersist flushes every write
Module.preRun = Module.preRun || [];
Module.preRun.push(function () {
  FS.mkdir('/save');
  FS.mount(IDBFS, { autoPersist: true }, '/save');
  addRunDependency('idbfs');
  FS.syncfs(true, function (err) {
    if (err) console.warn('save: initial sync failed', err);
    removeRunDependency('idbfs');
  });
});
