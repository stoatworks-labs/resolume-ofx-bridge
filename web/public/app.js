// Assemble a wrapped OpenFX plugin in the browser.
//
// This is the whole of the generator's job now that the shell describes itself
// (see the repo's docs/05-any-to-any.md): copy a prebuilt shell binary, copy
// the guest bundle beside it, write an Info.plist. The shell works out what it
// is carrying when the host loads it — which is why no native code has to run
// here, and nothing is uploaded anywhere.
//
// Deliberately no build step and no framework: a page whose entire argument is
// "you don't have to install anything" should be readable in one file.

const SHELL_URL = './shells/ffglofxshell-macos-universal.ofx';

const el = (id) => document.getElementById(id);
const log = (msg, cls = '') => {
  const d = document.createElement('div');
  d.textContent = msg;
  if (cls) d.className = cls;
  el('log').appendChild(d);
};

let guestHandle = null;   // FileSystemDirectoryHandle for the .bundle/.plugin
let guestName = '';       // e.g. "Tinsel.bundle"
let guestKind = 'ffgl';   // 'ffgl' | 'ae'
let outHandle = null;

if (!('showDirectoryPicker' in window)) {
  el('unsupported').hidden = false;
  el('unsupported').classList.add('unsupported');
  for (const id of ['pickGuest', 'pickOut', 'build']) el(id).disabled = true;
}

/** Read a text file out of a directory handle, or '' if it isn't there. */
async function readText(dir, ...path) {
  try {
    let h = dir;
    for (const part of path.slice(0, -1)) h = await h.getDirectoryHandle(part);
    const file = await (await h.getFileHandle(path[path.length - 1])).getFile();
    return await file.text();
  } catch {
    return '';
  }
}

/**
 * Which plugin format this is, from static data only.
 *
 * An After Effects plugin declares CFBundlePackageType eFKT in its own
 * Info.plist. That is the same discriminator the shell itself uses at load
 * time, so the page and the plugin cannot disagree about what was wrapped.
 */
export async function sniffKind(dir) {
  const plist = await readText(dir, 'Contents', 'Info.plist');
  return plist.includes('eFKT') ? 'ae' : 'ffgl';
}

/** Copy a directory tree from one handle to another, recursively. */
async function copyTree(src, dstParent, name, onFile) {
  const dst = await dstParent.getDirectoryHandle(name, { create: true });
  for await (const [childName, handle] of src.entries()) {
    if (handle.kind === 'directory') {
      await copyTree(handle, dst, childName, onFile);
    } else {
      const file = await handle.getFile();
      const out = await dst.getFileHandle(childName, { create: true });
      const w = await out.createWritable();
      await w.write(await file.arrayBuffer());
      await w.close();
      if (onFile) onFile(file.size);
    }
  }
  return dst;
}

async function writeFile(dir, name, contents) {
  const h = await dir.getFileHandle(name, { create: true });
  const w = await h.createWritable();
  await w.write(contents);
  await w.close();
}

el('pickGuest').addEventListener('click', async () => {
  try {
    const dir = await window.showDirectoryPicker({ id: 'guest', mode: 'read' });
    // The picker hands back the folder itself, so its name is the bundle name.
    if (!/\.(bundle|plugin|ofx\.bundle)$/i.test(dir.name)) {
      el('guestInfo').innerHTML =
        `<b>${dir.name}</b> doesn't look like a plugin bundle — expected a folder ` +
        `ending .bundle or .plugin.`;
      return;
    }
    guestHandle = dir;
    guestName = dir.name;
    guestKind = await sniffKind(dir);

    const label = guestKind === 'ae' ? 'After Effects plugin' : 'FFGL plugin';
    el('guestInfo').innerHTML =
      `<b>${guestName}</b><span class="badge">${label}</span>`;
    el('step1row').classList.add('done');
    el('pickOut').disabled = false;
  } catch (e) {
    if (e.name !== 'AbortError') el('guestInfo').textContent = `Couldn't read that: ${e.message}`;
  }
});

el('pickOut').addEventListener('click', async () => {
  try {
    outHandle = await window.showDirectoryPicker({ id: 'out', mode: 'readwrite' });
    el('outInfo').innerHTML = `Writing into <b>${outHandle.name}</b>`;
    el('step2row').classList.add('done');
    el('build').disabled = false;
  } catch (e) {
    if (e.name !== 'AbortError') el('outInfo').textContent = `Couldn't use that folder: ${e.message}`;
  }
});

/**
 * Assemble the wrapped bundle. Split out from the button handler so it can be
 * driven against any FileSystemDirectoryHandle — the test in web/test/ runs it
 * against the origin-private filesystem, which is the same interface the
 * picker returns.
 */
export async function assembleBundle(guest, name, kind, outDir, onLog = () => {}) {
  const stem = name.replace(/\.(ofx\.bundle|bundle|plugin)$/i, '');
  const safe = stem.replace(/[^A-Za-z0-9]+/g, '_');
  const suffix = kind === 'ae' ? '_AE' : '_FFGL';
  const bundleName = `${safe}${suffix}.ofx.bundle`;
  const binaryName = `${safe}${suffix}.ofx`;
  const identifier =
    `com.stoatworks.${kind === 'ae' ? 'aewrap' : 'ffglwrap'}.` + safe.toLowerCase();

  onLog('Fetching the shell…');
  const shell = await (await fetch(SHELL_URL)).arrayBuffer();

  const bundle = await outDir.getDirectoryHandle(bundleName, { create: true });
  const contents = await bundle.getDirectoryHandle('Contents', { create: true });
  const macos = await contents.getDirectoryHandle('MacOS', { create: true });
  const guestDir = await contents.getDirectoryHandle('Guest', { create: true });

  await writeFile(macos, binaryName, shell);
  onLog(`Shell written (${(shell.byteLength / 1048576).toFixed(1)} MB)`, 'ok');

  let bytes = 0;
  await copyTree(guest, guestDir, name, (n) => { bytes += n; });
  onLog(`Guest copied (${(bytes / 1048576).toFixed(1)} MB)`, 'ok');

  await writeFile(contents, 'Info.plist',
    `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
\t<key>CFBundleExecutable</key>\t<string>${binaryName}</string>
\t<key>CFBundleIdentifier</key>\t<string>${identifier}</string>
\t<key>CFBundlePackageType</key>\t<string>BNDL</string>
</dict>
</plist>
`);
  onLog('Info.plist written', 'ok');

  // No manifest is written. The shell reads the guest's own Info.plist to tell
  // an After Effects plugin from an FFGL one, and asks it for its parameters
  // when the host loads it.
  onLog(`${bundleName} is ready`, 'ok');
  return bundleName;
}

el('build').addEventListener('click', async () => {
  el('build').disabled = true;
  el('log').innerHTML = '';
  el('after').hidden = true;

  try {
    const bundleName = await assembleBundle(guestHandle, guestName, guestKind, outHandle, log);
    const stem = guestName.replace(/\.(ofx\.bundle|bundle|plugin)$/i, '');
    // No path is available from the File System Access API — only the folder's
    // name — so the command shows that and invites the drag, which is how a
    // person gets a real path into Terminal anyway.
    el('installCmd').textContent =
      `# tip: type the command, then drag ${bundleName} from Finder onto the window\n` +
      `xattr -dr com.apple.quarantine <${outHandle.name}>/${bundleName}\n` +
      `sudo cp -R <${outHandle.name}>/${bundleName} /Library/OFX/Plugins/`;
    el('after').hidden = false;
    el('step3row').classList.add('done');
  } catch (e) {
    log(`Failed: ${e.message}`, 'err');
  } finally {
    el('build').disabled = false;
  }
});
