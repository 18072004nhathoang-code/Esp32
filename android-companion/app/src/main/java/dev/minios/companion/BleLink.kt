package dev.minios.companion

import android.annotation.SuppressLint
import android.bluetooth.*
import android.content.Context
import java.util.UUID
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

@SuppressLint("MissingPermission")
class BleLink(private val context:Context,private val state:(Boolean,String)->Unit) {
    private fun uuid(n:Int)=UUID.fromString("7cf1000$n-6e6d-4f73-9f2e-455333433238")
    private var gatt:BluetoothGatt?=null
    private val replies=LinkedBlockingQueue<Pair<Int,ByteArray>>()
    @Volatile var ready=false;private set
    @Volatile var mtu=23;private set
    private var service:BluetoothGattService?=null
    private val callback=object:BluetoothGattCallback(){
        override fun onConnectionStateChange(g:BluetoothGatt,status:Int,newState:Int) {
            if(g!==gatt)return
            if(status==BluetoothGatt.GATT_SUCCESS && newState==BluetoothProfile.STATE_CONNECTED) {
                if(!g.discoverServices())state(false,"Không đọc được dịch vụ BLE")
            } else {ready=false;replies.offer(-1 to byteArrayOf());state(false,"Mất kết nối BLE ($status)")}
        }
        override fun onServicesDiscovered(g:BluetoothGatt,status:Int) {
            if(g!==gatt)return
            service=g.getService(uuid(0))
            if(status!=0 || (3..5).any{service?.getCharacteristic(uuid(it))==null}) {state(false,"Firmware chưa hỗ trợ chỉ đường");return}
            if(!g.requestMtu(247)) {ready=true;state(true,"Đã kết nối; đang xác thực")}
        }
        override fun onMtuChanged(g:BluetoothGatt,value:Int,status:Int) {
            if(g!==gatt)return
            mtu=if(status==0)value else 23;ready=true;state(true,"Đã kết nối; nhập mã trên ESP32 nếu được hỏi")
        }
        override fun onCharacteristicWrite(g:BluetoothGatt,c:BluetoothGattCharacteristic,status:Int) {if(g===gatt)replies.offer(status to byteArrayOf())}
        @Deprecated("Compatibility callback for Android 8+")
        override fun onCharacteristicRead(g:BluetoothGatt,c:BluetoothGattCharacteristic,status:Int) {if(g===gatt)replies.offer(status to c.value.clone())}
    }
    fun connect(address:String) {
        close();val adapter=context.getSystemService(BluetoothManager::class.java).adapter
        gatt=adapter.getRemoteDevice(address).connectGatt(context,false,callback,BluetoothDevice.TRANSPORT_LE)
    }
    // All I/O is invoked from the bridge's one executor: never concurrent GATT operations.
    fun write(n:Int,bytes:ByteArray) {
        check(ready){"BLE chưa sẵn sàng"};val g=gatt?:error("BLE đã đóng")
        val c=service?.getCharacteristic(uuid(n))?:error("Thiếu characteristic")
        replies.clear();c.writeType=BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT;c.value=bytes
        check(g.writeCharacteristic(c)){"BLE bận"}
        val result=replies.poll(10,TimeUnit.SECONDS)?:run{close();error("BLE timeout")}
        check(result.first==0){"ESP32 từ chối gói tin (${result.first}); kiểm tra ghép đôi"}
    }
    fun ack():ByteArray {
        val g=gatt?:error("BLE đã đóng");val c=service?.getCharacteristic(uuid(5))?:error("Thiếu ACK")
        replies.clear();check(g.readCharacteristic(c)){"Không đọc được ACK"}
        val r=replies.poll(10,TimeUnit.SECONDS)?:run{close();error("ACK timeout")}
        check(r.first==0 && r.second.size==20 && r.second[0].toInt()==1){"ACK không hợp lệ hoặc chưa ghép đôi"}
        return r.second
    }
    fun close(){ready=false;service=null;val old=gatt;gatt=null;old?.disconnect();old?.close();replies.offer(-1 to byteArrayOf())}
}
