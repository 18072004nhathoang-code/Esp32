export const UUID = Object.freeze({
  service: '7cf10000-6e6d-4f73-9f2e-455333433238',
  command: '7cf10001-6e6d-4f73-9f2e-455333433238',
  status: '7cf10002-6e6d-4f73-9f2e-455333433238',
});
export const OP = Object.freeze({play:1, pause:2, resume:3, stop:4, volume:5, status:6, next:7, previous:8});
export function encodeCommand(op, id, value = 0) {
  if (!Number.isInteger(op) || op < 1 || op > 8 || !Number.isInteger(id) || id < 1 || id > 65535 ||
      !Number.isInteger(value) || value < 0 || value > 65535 || (op === 5 && value > 100) ||
      (op !== 1 && op !== 5 && value !== 0)) throw new Error('Lệnh BLE không hợp lệ.');
  const data = new DataView(new ArrayBuffer(6));
  data.setUint8(0,1); data.setUint8(1,op); data.setUint16(2,id,true); data.setUint16(4,value,true);
  return data;
}
export function decodeStatus(data) {
  if (!(data instanceof DataView) || data.byteLength !== 20 || data.getUint8(0) !== 1 ||
      data.getUint8(4) > 5 || data.getUint8(5) > 100) throw new Error('Trạng thái BLE sai giao thức.');
  const flags = data.getUint8(1);
  const track = data.getInt8(6), tracks = data.getUint8(7);
  // Version 1 has six flag bits and the firmware's SD playlist has 64 slots.
  // Reject corrupt/unsupported snapshots before they can settle a command.
  if ((flags & 0xc0) || track < -1 || track > 63 || tracks > 64)
    throw new Error('Trạng thái BLE sai giao thức.');
  return {id:data.getUint16(2,true), result:data.getUint8(4), playing:!!(flags&1), paused:!!(flags&2),
    wifi:!!(flags&4), sd:!!(flags&8), busy:!!(flags&16), available:!(flags&32), volume:data.getUint8(5),
    track, tracks, position:data.getUint32(8,true),
    duration:data.getUint32(12,true), session:data.getUint32(16,true)};
}
export const resultMessage = Object.freeze([
  'Trạng thái thiết bị', 'Thiết bị đã thực hiện lệnh', 'Lệnh/bài hát không hợp lệ',
  'AI đang dùng âm thanh. Chờ lượt hỏi hoàn tất rồi thử lại.',
  'Chưa có ACK thành công. Kiểm tra trạng thái; không gửi lại tự động.',
  'Nhạc/thẻ SD không khả dụng. Kiểm tra trên ESP32.',
]);

export class Remote {
  constructor(onState = () => {}, onDisconnect = () => {}, timeoutMs = 8000) {
    this.onState=onState; this.onDisconnect=onDisconnect; this.timeoutMs=timeoutMs;
    this.generation=0; this.device=null; this.pending=null; this.id=0; this.session=null;
  }
  async connect(bluetooth) {
    this.disconnect();
    const generation=++this.generation;
    const live=()=> { if (generation!==this.generation) throw new Error('Kết nối đã hủy.'); };
    try {
      if (!bluetooth) throw new Error('Trình duyệt không hỗ trợ Web Bluetooth. Dùng ứng dụng BLE GATT.');
      const device=await bluetooth.requestDevice({filters:[{services:[UUID.service]}]});
      live(); this.device=device;
      this.disconnected=()=> { if (generation===this.generation) { this.disconnect(); this.onDisconnect(); } };
      device.addEventListener('gattserverdisconnected',this.disconnected);
      const server=await device.gatt.connect(); live();
      const service=await server.getPrimaryService(UUID.service); live();
      this.command=await service.getCharacteristic(UUID.command); live();
      this.status=await service.getCharacteristic(UUID.status); live();
      this.changed=event=> {
        if (generation===this.generation) {
          try { this.receive(event.target.value); }
          catch { this.disconnect(); this.onDisconnect(); }
        }
      };
      this.status.addEventListener('characteristicvaluechanged',this.changed);
      // Protected read triggers OS pairing: enter the fresh six-digit ESP32 code.
      const first=await this.status.readValue(); live();
      const state=decodeStatus(first); this.id=state.id; this.session=state.session; this.receive(first);
      await this.status.startNotifications(); live();
      return state;
    } catch (error) {
      if (generation===this.generation) this.disconnect();
      throw error;
    }
  }
  receive(data) {
    const state=decodeStatus(data);
    if (this.session===null || state.session!==this.session) return state;
    this.onState(state);
    if (this.pending && state.id===this.pending.id) {
      // Status is observational, not proof that Music acknowledged a mutation.
      // Keep waiting for Applied/error without replaying the command.
      if (state.result===0 && this.pending.op!==OP.status) return state;
      const pending=this.pending; this.pending=null; clearTimeout(pending.timer);
      if (state.result===1 || state.result===0) pending.resolve(state);
      else pending.reject(new Error(resultMessage[state.result]));
    }
    return state;
  }
  async refresh() {
    const generation=this.generation;
    if (!this.status || !this.device?.gatt.connected) throw new Error('Chưa kết nối ESP32.');
    const data=await this.status.readValue();
    if (generation!==this.generation) throw new Error('Kết nối đã hủy.');
    return this.receive(data);
  }
  async send(op, value = 0) {
    if (!this.command || !this.device?.gatt.connected) throw new Error('Chưa kết nối ESP32.');
    if (this.pending) throw new Error('Đang chờ ACK của lệnh trước.');
    if (this.id===65535) throw new Error('Đã hết ID phiên. Ngắt rồi kết nối lại.');
    const id=++this.id, packet=encodeCommand(op,id,value);
    const generation=this.generation, command=this.command;
    return new Promise((resolve,reject)=>{
      const pending={id,op,resolve,reject,timer:null}; this.pending=pending;
      pending.timer=setTimeout(()=>{
        // Recover a dropped notification with a protected read. Never replay a
        // Play/Stop command: an ACK timeout does not prove it wasn't executed.
        if (this.pending===pending) {
          this.pending=null; reject(new Error('Hết thời gian chờ ACK. Kiểm tra trạng thái trước khi gửi lệnh khác.'));
          if (generation===this.generation) void this.refresh().catch(()=>{});
        }
      },this.timeoutMs);
      command.writeValueWithResponse(packet).catch(error=>{
        if (generation===this.generation && this.pending===pending) {
          this.pending=null; clearTimeout(pending.timer); reject(error);
        }
      });
    });
  }
  disconnect() {
    ++this.generation;
    if (this.pending) {
      clearTimeout(this.pending.timer); this.pending.reject(new Error('Đã ngắt kết nối. Lệnh đang xử lý có thể đã được thực hiện.'));
      this.pending=null;
    }
    if (this.status && this.changed) this.status.removeEventListener('characteristicvaluechanged',this.changed);
    if (this.device && this.disconnected) this.device.removeEventListener('gattserverdisconnected',this.disconnected);
    this.device?.gatt.disconnect();
    this.device=this.command=this.status=null; this.id=0; this.session=null;
  }
}
