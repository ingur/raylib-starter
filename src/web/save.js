// Web save support. /save is backed by IndexedDB and mounted before main()
// runs, so save files are readable at boot. autoPersist flushes every write.
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
