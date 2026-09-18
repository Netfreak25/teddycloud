// Focused checks against the actual settings handler; no server or new test dependencies.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import vm from 'node:vm';

const web = new URL('../teddycloud_web/', import.meta.url);
const require = createRequire(new URL('package.json', web));
const ts = require('typescript');
const pending = [];
const writes = [];
let configWrites = 0;
const api = {
    apiGetTeddyCloudSettingRaw: (...args) => new Promise(resolve => pending.push({ args, resolve })),
    apiPostTeddyCloudSetting: async (...args) => { writes.push(args); },
    apiTriggerWriteConfigGet: async () => { configWrites++; },
};
const source = fs.readFileSync(new URL('src/data/SettingsDataHandler.ts', web), 'utf8');
const compiled = ts.transpileModule(source, {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
}).outputText;
const exports = {};
vm.runInNewContext(compiled, {
    exports, console,
    require: name => {
        if (name === 'i18next') return { t: key => key };
        if (name.endsWith('/TeddyCloudApi')) return { TeddyCloudApi: class { constructor() { return api; } } };
        if (name.endsWith('/defaultApiConfig')) return { defaultAPIConfig: () => ({}) };
        if (name.endsWith('/teddyCloudNotificationTypes')) return { NotificationTypeEnum: { Success: 'success', Error: 'error' } };
        throw new Error(`Unexpected dependency: ${name}`);
    },
});
const handler = exports.default.initialize((type, ...message) => {
    assert.notEqual(type, 'error', message.join(' '));
}, key => key);
const id = 'mqtt_client_upstream.forward.playback.state';
const otherId = 'mqtt_client_upstream.forward.logs.other';
const setting = (iD, value, overlayed) => ({ iD, value, overlayed, type: 'bool', label: iD });
const flushGlobal = async (expectedId, value) => {
    const request = pending.shift();
    assert.deepEqual(request.args, [expectedId]); // global read, never another box's value
    request.resolve({ text: async () => String(value) });
    await new Promise(resolve => setImmediate(resolve));
};

// Reset removes the override even when the global value equals the previous value.
handler.initializeSettings([setting(id, false, true), setting(otherId, true, true)], 'BOX-A');
handler.changeSettingOverlayed(id, false);
handler.changeSettingOverlayed(otherId, false);
assert.equal(writes.length, 0, 'reset must not write before Save');
await flushGlobal(id, false);
await flushGlobal(otherId, false);
assert.equal(handler.getSetting(otherId).value, false);
assert.equal(handler.hasUnchangedChanges(), true);
await handler.saveAll();
assert.deepEqual(writes.splice(0), [[id, false, 'BOX-A', true], [otherId, false, 'BOX-A', true]]);
assert.equal(configWrites, 1);
assert.equal(handler.hasUnchangedChanges(), false);

// Enabling an override keeps the inherited value until explicitly edited.
handler.changeSettingOverlayed(id, true);
assert.equal(handler.getSetting(id).value, false);
handler.changeSetting(id, true, true);
await handler.saveAll();
assert.deepEqual(writes.splice(0), [[id, true, 'BOX-A', false]]);

// Discard undoes a reset, including one whose global read is still pending.
handler.changeSettingOverlayed(id, false);
handler.resetAll();
await flushGlobal(id, false);
assert.equal(handler.getSetting(id).overlayed, true);
assert.equal(handler.getSetting(id).value, true);
assert.equal(handler.hasUnchangedChanges(), false);
assert.equal(writes.length, 0);

// A late global response cannot overwrite a newly edited override.
handler.changeSettingOverlayed(id, false);
handler.changeSettingOverlayed(id, true);
handler.changeSetting(id, true, true);
await flushGlobal(id, false);
assert.equal(handler.getSetting(id).value, true);
assert.equal(handler.getSetting(id).overlayed, true);

// Nor may it replace the same setting in a subsequently opened box dialog.
handler.changeSettingOverlayed(id, false);
handler.initializeSettings([setting(id, true, false)], 'BOX-B');
await flushGlobal(id, false);
assert.equal(handler.getSetting(id).value, true);
assert.equal(handler.getSetting(id).overlayId, 'BOX-B');

// The global settings view retains its ordinary write behavior.
handler.initializeSettings([setting(id, true, undefined)], undefined);
handler.changeSetting(id, false, undefined);
await handler.saveAll();
assert.deepEqual(writes.splice(0), [[id, false, undefined, false]]);
assert.equal(pending.length, 0);

// Toggling the master must never overwrite the saved individual rules.
const masterId = 'mqtt_client_upstream.filters_enabled';
handler.initializeSettings([setting(masterId, true, undefined), setting(id, false, undefined)], undefined);
for (const enabled of [false, true]) {
    handler.changeSetting(masterId, enabled, undefined);
    assert.equal(handler.getSetting(id).value, false);
    await handler.saveAll();
    assert.deepEqual(writes.splice(0), [[masterId, enabled, undefined, false]]);
}

// Keep the new controls scoped to the entire filter section, not search results.
const component = fs.readFileSync(new URL('src/components/common/form/MqttForwardingFilters.tsx', web), 'utf8');
const reset = component.slice(component.indexOf('const resetOverrides ='), component.indexOf('const changeGlobal ='));
assert.match(reset, /settings\s*\.filter\(/);
assert.match(reset, /!setting.readOnly/);
assert.match(reset, /handler.changeSettingOverlayed\(setting.iD, false\)/);
assert.doesNotMatch(reset, /saveAll|changeSetting\(|visibleGroups/);
assert.match(component, /checked=\{setting.overlayed === true\}/);
assert.match(component, /t\("settings.overlayed"\)/);
assert.doesNotMatch(component, /settings.mqttForwarding.useOverride/);
assert.match(component, /<SettingsOptionItem iD=\{MQTT_FILTERS_ENABLED\} noOverlay/);
assert.match(component, /handler.getSetting\(MQTT_FILTERS_ENABLED\)\?\.value !== false/);
assert.match(component, /onChange=\{\(checked\) =>\s*handler.changeSettingOverlayed\(setting.iD, checked\)\s*\}/);
assert.match(component, /overlayId !== undefined && \(\s*<Tooltip title=\{t\("settings.mqttForwarding.resetToGlobalHint"\)/);
for (const language of ['de', 'en', 'fr', 'es', 'tlh']) {
    const translations = JSON.parse(fs.readFileSync(new URL(`public/translations/${language}.json`, web), 'utf8'));
    // Klingon uses the same existing English fallback as ordinary settings.
    const english = JSON.parse(fs.readFileSync(new URL('public/translations/en.json', web), 'utf8'));
    assert.ok(translations.settings.overlayed || english.settings.overlayed, `${language}: standard override label`);
    assert.ok(translations.settings.optionText.mqtt_client_upstream__filters_enabled.label);
    for (const key of ['overrideHint', 'resetToGlobal', 'resetToGlobalHint', 'disabledGlobally']) {
        assert.ok(translations.settings.mqttForwarding[key], `${language}: ${key}`);
    }
}
console.log('MQTT filter reset, override/save/discard, global inheritance, late-response guards and UI wiring passed.');
