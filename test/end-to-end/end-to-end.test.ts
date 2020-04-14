import glob from 'glob';
import * as path from 'path';
import fs from 'fs-extra';
import { createVirtualMachine, VirtualMachineWithMembrane } from '../../lib/virtual-machine-proxy';
import { snapshotToBytecode, stringifySnapshot } from '../../lib/snapshot';
import { htmlTemplate as htmlPageTemplate } from '../../lib/general';

const testDir = './test/end-to-end/tests';
const rootArtifactDir = './test/end-to-end/artifacts';
const testFiles = glob.sync(testDir + '/**/*.test.mvms');

suite('end-to-end', function () {
  for (let filename of testFiles) {
    const testFilenameFull = path.resolve(filename);
    const testFilenameRelativeToTestDir = path.relative(testDir, testFilenameFull);
    const testFilenameRelativeToCurDir = './' + path.relative(process.cwd(), testFilenameFull).replace(/\\/g, '/');
    const testFriendlyName = testFilenameRelativeToTestDir.slice(0, -10);
    const testArtifactDir = path.resolve(rootArtifactDir, testFilenameRelativeToTestDir.slice(0, -10));
    fs.ensureDirSync(testArtifactDir);

    test(testFriendlyName, () => {
      // ----------------------- Create Comprehensive VM ----------------------

      const printLog: string[] = [];
      const globals = {
        print: () => {}, // TODO
        vmExport(id: number, fn: any) {
          vm.exportValue(id, fn);
        }
      };
      const vm = new VirtualMachineWithMembrane(globals);

      // ----------------------------- Load Source ----------------------------

      const src = fs.readFileSync(testFilenameRelativeToCurDir, 'utf8')
      vm.importModuleSourceText(src, path.basename(testFilenameRelativeToCurDir));

      const postLoadSnapshot = vm.createSnapshot();
      fs.writeFileSync(path.resolve(testArtifactDir, '1.post-load.snapshot'), stringifySnapshot(postLoadSnapshot), null);
      const { bytecode: postLoadBytecode, html: postLoadHTML } = snapshotToBytecode(postLoadSnapshot, true);
      fs.writeFileSync(path.resolve(testArtifactDir, '1.post-load.mvm-bc'), postLoadBytecode, null);
      fs.writeFileSync(path.resolve(testArtifactDir, '1.post-load.mvm-bc.html'), htmlPageTemplate(postLoadHTML!), null);

      // --------------------------- Garbage Collect --------------------------

      vm.garbageCollect();

      const postGarbageCollectSnapshot = vm.createSnapshot();
      fs.writeFileSync(path.resolve(testArtifactDir, '2.post-gc.snapshot'), stringifySnapshot(postGarbageCollectSnapshot), null);
      const { bytecode: postGarbageCollectBytecode, html: postGarbageCollectHTML } = snapshotToBytecode(postGarbageCollectSnapshot, true);
      fs.writeFileSync(path.resolve(testArtifactDir, '2.post-gc.mvm-bc'), postGarbageCollectBytecode, null);
      fs.writeFileSync(path.resolve(testArtifactDir, '2.post-gc.mvm-bc.html'), htmlPageTemplate(postGarbageCollectHTML!), null);
    });
  }
});