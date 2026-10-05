import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {Remote,encodeCommand,decodeStatus,OP,UUID} from '../tools/ble-remote/remote.mjs';

function status(id=0,result=0,session=1){
 const view=new DataView(new ArrayBuffer(20));
 view.setUint8(0,1);view.setUint16(2,id,true);view.setUint8(4,result);view.setUint32(16,session,true);return view;
}
class Characteristic extends EventTarget{
 constructor(){super();this.value=status();this.writes=[]}
 async readValue(){return this.value}
 async startNotifications(){return this}
 async writeValueWithResponse(packet){this.writes.push(packet)}
 notify(value){this.value=value;this.dispatchEvent(new Event('characteristicvaluechanged'))}
}
function radio(){
 const command=new Characteristic(),state=new Characteristic(),device=new EventTarget();let epoch=0;
 const gatt={connected:false,async connect(){this.connected=true;state.value=status(0,0,++epoch);return this},disconnect(){this.connected=false},
  async getPrimaryService(uuid){assert.equal(uuid,UUID.service);return {getCharacteristic:async id=>id===UUID.command?command:state}}};
 device.gatt=gatt;
 return {command,state,device,epoch:()=>epoch,bluetooth:{requestDevice:async options=>{assert.deepEqual(options,{filters:[{services:[UUID.service]}]});return device}}};
}
test('binary contract and strict validation match firmware',()=>{
 assert.deepEqual([...new Uint8Array(encodeCommand(OP.volume,0x1234,100).buffer)],[1,5,0x34,0x12,100,0]);
 for(const args of [[0,1],[9,1],[1,0],[1,65536],[5,1,101],[4,1,1],[1,1,1.5]])assert.throws(()=>encodeCommand(...args));
 assert.throws(()=>decodeStatus(new DataView(new ArrayBuffer(19))));
 let view=status();view.setUint8(0,2);assert.throws(()=>decodeStatus(view));
 view=status();view.setUint8(4,6);assert.throws(()=>decodeStatus(view));
 view=status(0,0,0xabcdef12);view.setInt8(6,-1);
 assert.equal(decodeStatus(view).track,-1);assert.equal(decodeStatus(view).session,0xabcdef12);
 const fixture=readFileSync(new URL('fixtures/ble_status_v1.txt',import.meta.url),'utf8').trim().split(/\s+/).map(hex=>parseInt(hex,16));
 const golden=decodeStatus(new DataView(Uint8Array.from(fixture).buffer));
 assert.deepEqual(golden,{id:0x1234,result:4,playing:true,paused:false,wifi:true,sd:true,busy:false,available:true,
  volume:50,track:2,tracks:4,position:0x12345678,duration:90,session:0xabcdef12});
});
test('Status rejects reserved flags and out-of-contract playlist fields',()=>{
 for(const [offset,value] of [[1,64],[1,128],[5,101],[6,64],[6,128],[6,254],[7,65]]){
  const view=status();view.setUint8(offset,value);
  assert.throws(()=>decodeStatus(view),/sai giao thức/);
 }
 const view=status();view.setUint8(7,64);view.setInt8(6,63);view.setUint8(5,100);
 assert.equal(decodeStatus(view).track,63);assert.equal(decodeStatus(view).tracks,64);
 view.setInt8(6,-1);assert.equal(decodeStatus(view).track,-1);
});
test('malformed notification cannot acknowledge a pending Music command',async()=>{
 const r=radio();let disconnected=0;
 const remote=new Remote(()=>{},()=>disconnected++);await remote.connect(r.bluetooth);
 const failure=assert.rejects(remote.send(OP.play,0),/Đã ngắt/);
 const frame=status(1,1);frame.setUint8(7,65);
 r.state.notify(frame);await failure;
 assert.equal(disconnected,1);assert.equal(r.device.gatt.connected,false);
 assert.equal(r.command.writes.length,1);
});
test('write acceptance is not success; only matching decoder ACK settles the command',async()=>{
 const r=radio(),remote=new Remote();await remote.connect(r.bluetooth);
 const pending=remote.send(OP.play,0);let resolved=false;pending.then(()=>resolved=true);
 await Promise.resolve();assert.equal(resolved,false);assert.equal(r.command.writes.length,1);
 r.state.notify(status(9,1));await Promise.resolve();assert.equal(resolved,false);
 // A status response is not a decoder ACK, even with the same command ID.
 r.state.notify(status(1,0));
 await new Promise(resolve=>setImmediate(resolve));assert.equal(resolved,false);
 r.state.notify(status(1,1));assert.equal((await pending).id,1);remote.disconnect();
});
test('Status command accepts the firmware Status result without invoking playback',async()=>{
 const r=radio(),remote=new Remote();await remote.connect(r.bluetooth);
 const pending=remote.send(OP.status);
 r.state.notify(status(1,0));assert.equal((await pending).result,0);
 assert.equal(r.command.writes.length,1);
 assert.equal(r.command.writes[0].getUint8(1),OP.status);remote.disconnect();
});
test('busy command, device refusal and disconnect never report success',async()=>{
 const r=radio(),remote=new Remote();await remote.connect(r.bluetooth);
 let first=remote.send(OP.stop);await assert.rejects(remote.send(OP.pause),/ACK/);
 r.state.notify(status(1,3));await assert.rejects(first,/AI đang/);
 first=remote.send(OP.resume);remote.disconnect();await assert.rejects(first,/Đã ngắt/);
 await remote.connect(r.bluetooth);r.state.notify(status(2,1));
 first=remote.send(OP.volume,20);
 let done=false;first.catch(()=>done=true);
 r.state.notify(status(1,1,1));await Promise.resolve();assert.equal(done,false); // same ID, old wire session
 r.state.notify(status(1,4,r.epoch()));await assert.rejects(first,/Chưa có ACK/);remote.disconnect();
});
test('bounded timeout does not replay a command, even if recovery read hangs',async()=>{
 const r=radio(),remote=new Remote(()=>{},()=>{},10);await remote.connect(r.bluetooth);
 r.state.readValue=()=>new Promise(()=>{});
 await assert.rejects(remote.send(OP.play,0),/Hết thời gian/);
 assert.equal(r.command.writes.length,1);remote.disconnect();
});
test('cancel connection while chooser is open leaves no connection',async()=>{
 const r=radio(),remote=new Remote();let finish;
 const pending=remote.connect({requestDevice:()=>new Promise(resolve=>finish=resolve)});
 remote.disconnect();finish(r.device);await assert.rejects(pending,/đã hủy/);
 assert.equal(r.device.gatt.connected,false);
});
