// Focused executable checks; uses the WebUI's existing TypeScript dependency.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { createRequire } from 'node:module';
const require = createRequire(new URL('../teddycloud_web/package.json', import.meta.url));
const ts = require('typescript');
const web = new URL('../teddycloud_web/', import.meta.url);
const moduleUrl = (path, rewrite = code => code) => 'data:text/javascript;base64,' + Buffer.from(rewrite(ts.transpileModule(
    fs.readFileSync(new URL(path, web), 'utf8'),
    { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext } },
).outputText)).toString('base64');
const images = await import(moduleUrl('src/components/tonies/common/utils/imagePathUtils.ts'));
const upload = await import(moduleUrl('src/utils/images/prepareTonieImage.ts'));
const custom = '/api/tonie/image/8194F21C500304E0';
for (const unknown of [undefined, '', ' ', '/img_unknown.png', 'https://tc/img_unknown.png?v=1#x']) {
    assert.equal(images.isUnknownPicture(unknown), true);
    assert.equal(images.resolveContentDisplayPicture(unknown, custom, '/model.png'), custom);
}
assert.equal(images.resolveContentDisplayPicture('/cover.png', custom, '/model.png'), '/cover.png');
assert.equal(images.resolveTonieDisplayPicture(custom, '/model.png'), custom);
assert.equal(images.resolveContentDisplayPicture(undefined, undefined, '/model.png'), '/model.png');
assert.equal(images.resolveContentDisplayPicture(), '/img_unknown.png');
assert.equal(images.isUnknownPicture('/api/tonie/image/123?name=img_unknown.png'), false);
const remoteImages = await import(moduleUrl('src/components/tonies/common/utils/imagePathUtils.ts', code =>
    code.replaceAll('import.meta', '({ env: { VITE_APP_TEDDYCLOUD_API_URL: "https://tc:8443/" } })')));
assert.equal(remoteImages.toImageSrc(custom), 'https://tc:8443' + custom);
assert.equal(remoteImages.toImageSrc('custom_img/cover.png'), 'https://tc:8443/custom_img/cover.png');
assert.equal(remoteImages.toImageSrc('https://cdn/cover.png'), 'https://cdn/cover.png');
const native = await import(moduleUrl('src/utils/audio/nativeCollection.ts', code => code
    .replaceAll('import.meta', '({ env: {} })')
    .replace('"jszip"', JSON.stringify(new URL('node_modules/jszip/lib/index.js', web).href))
    .replace('"../../components/tonies/common/utils/imagePathUtils"',
        JSON.stringify(moduleUrl('src/components/tonies/common/utils/imagePathUtils.ts')))));
const collection = { contentHash: 'a'.repeat(64), chapterCount: 1, chapters: [ { path: 'one.opus', originalName: 'one' } ] };
const itemA = native.nativeCollectionToPlaybackItem(collection, '', { tonieRuid: 'A', picture: custom });
const itemB = native.nativeCollectionToPlaybackItem(collection, '', { tonieRuid: 'B', picture: '/api/tonie/image/B' });
const libraryItem = native.nativeCollectionToPlaybackItem(collection);
assert.equal(itemA.id, itemB.id);
assert.deepEqual(itemA.sources, itemB.sources);
assert.notEqual(itemA.picture, itemB.picture);
assert.equal(libraryItem.picture, '/img_unknown.png');
assert.equal(libraryItem.tonieRuid, undefined);
assert.deepEqual(upload.tonieImageBounds(800, 400), { x: 0, y: 128, width: 512, height: 256 });
assert.deepEqual(upload.tonieImageBounds(200, 400), { x: 128, y: 0, width: 256, height: 512 });
assert.deepEqual(upload.tonieImageBounds(32, 32), { x: 0, y: 0, width: 512, height: 512 });
await assert.rejects(upload.prepareTonieImage(new File(['invalid'], 'bad.txt')), /InvalidFormat/);
await assert.rejects(upload.prepareTonieImage(new File([new Uint8Array(upload.TONIE_USER_IMAGE_MAX_SIZE + 1)], 'huge.png')), /TooLarge/);
console.log('Image priority, URL placeholders, proportional fitting and input limits passed.');

// Optional real browser verification, using an already installed Playwright and browser.
// TONIE_IMAGE_PLAYWRIGHT_MODULE points to playwright-core; no package installation needed.
if (process.argv.includes('--browser')) {
    const { chromium } = require(process.env.TONIE_IMAGE_PLAYWRIGHT_MODULE || 'playwright-core');
    const browser = await chromium.launch({ executablePath: process.env.TONIE_IMAGE_BROWSER, headless: true });
    try {
        const page = await browser.newPage();
        const results = await page.evaluate(async (module) => {
            const { prepareTonieImage } = await import(module);
            const inspect = async (file) => {
                const result = await prepareTonieImage(file);
                const bitmap = await createImageBitmap(result);
                const canvas = document.createElement('canvas');
                canvas.width = bitmap.width; canvas.height = bitmap.height;
                const ctx = canvas.getContext('2d');
                ctx.drawImage(bitmap, 0, 0); bitmap.close();
                const pixel = (x, y) => Array.from(ctx.getImageData(x, y, 1, 1).data);
                return { width: canvas.width, height: canvas.height, type: result.type,
                    center: pixel(256, 256), top: pixel(256, 0), left: pixel(0, 256), corner: pixel(0, 0) };
            };
            const png = async (w, h, transparent = false) => {
                const canvas = document.createElement('canvas'); canvas.width = w; canvas.height = h;
                const ctx = canvas.getContext('2d'); ctx.fillStyle = '#ff0000';
                ctx.fillRect(transparent ? w / 4 : 0, transparent ? h / 4 : 0,
                    transparent ? w / 2 : w, transparent ? h / 2 : h);
                const blob = await new Promise(resolve => canvas.toBlob(resolve, 'image/png'));
                return new File([blob], 'input.png', { type: 'image/png' });
            };
            // Two-frame GIF: first frame red, second blue. The decoder must freeze the first.
            const gif = new Uint8Array([
                ...Array.from('GIF89a', c => c.charCodeAt(0)), 1,0,1,0,128,0,0, 255,0,0, 0,0,255,
                33,249,4,0,10,0,0,0, 44,0,0,0,0,1,0,1,0,0,2,2,68,1,0,
                33,249,4,0,10,0,0,0, 44,0,0,0,0,1,0,1,0,0,2,2,76,1,0,59,
            ]);
            let badRejected = false;
            try { await prepareTonieImage(new File(['not an image'], 'broken.png')); }
            catch { badRejected = true; }
            return {
                landscape: await inspect(await png(800, 400)), portrait: await inspect(await png(200, 400)),
                transparent: await inspect(await png(32, 32, true)),
                gif: await inspect(new File([gif], 'animated.gif', { type: 'image/gif' })), badRejected,
            };
        }, moduleUrl('src/utils/images/prepareTonieImage.ts'));
        for (const key of ['landscape', 'portrait', 'transparent', 'gif']) {
            assert.equal(results[key].width, 512); assert.equal(results[key].height, 512);
            assert.equal(results[key].type, 'image/png');
            assert.deepEqual(results[key].center, [255, 0, 0, 255], key);
        }
        assert.equal(results.landscape.top[3], 0);
        assert.equal(results.portrait.left[3], 0);
        assert.equal(results.transparent.corner[3], 0);
        assert.equal(results.badRejected, true);
        console.log('Browser: 512x512 PNG, portrait/landscape, transparent padding, first GIF frame and decode failure passed.');
    } finally { await browser.close(); }
}
