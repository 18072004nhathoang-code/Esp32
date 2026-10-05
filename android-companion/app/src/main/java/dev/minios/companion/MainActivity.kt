package dev.minios.companion
import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.bluetooth.BluetoothManager
import android.bluetooth.le.*
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.*
import android.provider.Settings
import android.widget.*

@SuppressLint("MissingPermission")
class MainActivity:Activity(){
    private lateinit var list:LinearLayout
    private lateinit var status:TextView
    private val handler=Handler(Looper.getMainLooper())
    private val seen=HashSet<String>()
    private var scanner:BluetoothLeScanner?=null
    private val callback=object:ScanCallback(){
        override fun onScanResult(type:Int,result:ScanResult){
            val name=result.scanRecord?.deviceName?:return
            if(!name.startsWith("MiniOS-") || !seen.add(result.device.address))return
            runOnUiThread{button("Kết nối $name"){
                stopScan();startForegroundService(Intent(this@MainActivity,BridgeService::class.java).putExtra("address",result.device.address))
            }}
        }
        override fun onScanFailed(code:Int){NavBus.state="Không quét được BLE ($code)"}
    }
    private fun button(text:String,action:()->Unit){list.addView(Button(this).apply{this.text=text;setOnClickListener{action()}})}
    override fun onCreate(saved:Bundle?){
        super.onCreate(saved)
        window.decorView.setPadding(0,48,0,36)
        list=LinearLayout(this).apply{orientation=LinearLayout.VERTICAL;setPadding(24,24,24,24);setBackgroundColor(Color.rgb(18,19,25))}
        setContentView(ScrollView(this).apply{addView(list)})
        list.addView(TextView(this).apply{text="MiniOS Companion";textSize=26f;setTextColor(Color.WHITE)})
        status=TextView(this).apply{textSize=16f;setPadding(0,24,0,24)};list.addView(status)
        button("1. Cấp quyền đọc thông báo Maps"){startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))}
        button("2. Cài giọng tiếng Việt offline"){startActivity(Intent("com.android.settings.TTS_SETTINGS"))}
        button("3. Tìm ESP32"){scan()}
        button("Ngắt kết nối"){stopService(Intent(this,BridgeService::class.java))}
        list.addView(TextView(this).apply{text="Bật BLE trong Settings trên ESP32. Kết nối và nhập mã hiện trên ESP32. Sau đó mở Google Maps, chọn điểm đến và bắt đầu dẫn đường. Chỉ thông báo Maps được xử lý; không cần danh bạ, mic hoặc tài khoản cloud."})
        handler.post(object:Runnable{override fun run(){status.text=NavBus.state;handler.postDelayed(this,500)}})
    }
    private fun scan(){
        val permissions=if(Build.VERSION.SDK_INT>=31)mutableListOf(Manifest.permission.BLUETOOTH_SCAN,Manifest.permission.BLUETOOTH_CONNECT) else mutableListOf(Manifest.permission.ACCESS_FINE_LOCATION)
        if(Build.VERSION.SDK_INT>=33)permissions+=Manifest.permission.POST_NOTIFICATIONS
        val missing=permissions.filter{checkSelfPermission(it)!=PackageManager.PERMISSION_GRANTED}
        if(missing.isNotEmpty()){requestPermissions(missing.toTypedArray(),1);return}
        val adapter=getSystemService(BluetoothManager::class.java).adapter
        if(adapter==null || !adapter.isEnabled){NavBus.state="Hãy bật Bluetooth điện thoại";return}
        stopScan();seen.clear();scanner=adapter.bluetoothLeScanner
        scanner?.startScan(null,ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(),callback)
        NavBus.state="Đang tìm MiniOS trong 15 giây…";handler.postDelayed({stopScan()},15000)
    }
    private fun stopScan(){scanner?.stopScan(callback);scanner=null}
    override fun onDestroy(){stopScan();handler.removeCallbacksAndMessages(null);super.onDestroy()}
}
