import test from 'node:test';
import assert from 'node:assert/strict';
import {createServer} from '../server.js';

test('fixed BLE page assets, security headers, HEAD and no traversal',async t=>{
 const server=createServer();await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 t.after(()=>new Promise(resolve=>server.close(resolve)));
 const base=`http://127.0.0.1:${server.address().port}`;
 const page=await fetch(base+'/ble/');assert.equal(page.status,200);
 assert.match(page.headers.get('content-security-policy'),/connect-src 'none'/);
 assert.equal(page.headers.get('permissions-policy'),'bluetooth=(self)');
 assert.match(await page.text(),/Điều khiển BLE/);
 const module=await fetch(base+'/ble/remote.mjs');assert.equal(module.status,200);
 assert.match(await module.text(),/writeValueWithResponse/);
 const head=await fetch(base+'/ble/',{method:'HEAD'});assert.equal(head.status,200);assert.equal(await head.text(),'');
 const guide=await fetch(base+'/ble/guide.txt');assert.equal(guide.status,200);
 assert.match(await guide.text(),/7cf10000-6e6d-4f73-9f2e-455333433238/);
 for(const path of ['/ble/.env','/ble/%2e%2e%2f.env','/ble/unknown.mjs']){
  const res=await fetch(base+path);assert.equal(res.status,404);
 }
});
