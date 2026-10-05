package dev.minios.companion
import android.app.*
import android.content.Intent
import android.os.IBinder
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.SecureRandom
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

class BridgeService:Service(){
    private val io=Executors.newSingleThreadScheduledExecutor()
    private lateinit var link:BleLink
    private lateinit var speech:PhoneSpeech
    private var session=0;private var seq=1;private var cue=0;private var lastHeartbeat=0L
    private val generation=AtomicInteger()
    private val gate=SpeechGate()
    private var route=false;private var lastGuidance:Guidance?=null
    private var address="";private var retryAt=0L;private var retries=0
    @Volatile private var stopped=false
    private fun report(s:String){NavBus.state=s}
    private fun failed(e:Exception){report(e.message?:"Lỗi BLE");generation.incrementAndGet();speech.cancel();route=false;lastGuidance=null;link.close();retryAt=System.currentTimeMillis()+5000}
    override fun onCreate(){
        super.onCreate()
        getSystemService(NotificationManager::class.java).createNotificationChannel(NotificationChannel("bridge","Kết nối ESP32",NotificationManager.IMPORTANCE_LOW))
        val stop=PendingIntent.getService(this,2,Intent(this,BridgeService::class.java).setAction("STOP"),PendingIntent.FLAG_IMMUTABLE)
        startForeground(1,Notification.Builder(this,"bridge").setSmallIcon(android.R.drawable.ic_dialog_map).setContentTitle("Chỉ đường trên ESP32")
            .setContentText("Đang chuyển chỉ dẫn Google Maps qua BLE").addAction(Notification.Action.Builder(null,"Ngắt",stop).build()).build())
        speech=PhoneSpeech(this,::report)
        link=BleLink(this){ready,message ->
            report(message)
            if(!stopped)io.execute {
                if(ready) {
                    try{link.ack();retries=0;route=false;lastGuidance=null;gate.reset();NavBus.guidance?.let{update(it)}}
                    catch(e:Exception){report(e.message?:"Lỗi xác thực");link.close();retryAt=System.currentTimeMillis()+10000}
                }else{generation.incrementAndGet();speech.cancel();route=false;retryAt=System.currentTimeMillis()+minOf(30000L,1000L shl minOf(5,retries++))}
            }
        }
        NavBus.listener={g -> if(g==null){generation.incrementAndGet();speech.cancel()};if(!stopped)io.execute {try{if(g==NavBus.guidance)update(g)}catch(e:Exception){failed(e)}}}
        io.scheduleAtFixedRate({
            if(stopped)return@scheduleAtFixedRate
            try {
                if(link.ready && route){heartbeat();val a=link.ack();val sid=ByteBuffer.wrap(a,4,4).order(ByteOrder.LITTLE_ENDIAN).int
                    if(sid==session)report(when(a[1].toInt()){2->"ESP32 đang đọc hướng dẫn";3->"ESP32 đã đọc xong";7->"ESP32 lỗi phát âm thanh";else->"Đã kết nối bảo mật · nhận chỉ đường"})}
                else if(!link.ready && retryAt!=0L && System.currentTimeMillis()>=retryAt){retryAt=System.currentTimeMillis()+15000;link.connect(address)}
            }catch(e:Exception){failed(e)}
        },2,2,TimeUnit.SECONDS)
    }
    private fun send(kind:Int,id:Int,body:ByteArray=byteArrayOf(),valid:()->Boolean={true}) {
        for(p in NavProtocol.fragments(kind,session,id,body,link.mtu)) {
            check(valid()){ "Đã hủy hướng dẫn cũ" }
            if(kind==5 && System.currentTimeMillis()-lastHeartbeat>=2000)heartbeat()
            link.write(if(kind==5)4 else 3,p)
        }
    }
    private fun heartbeat(){send(3,seq);lastHeartbeat=System.currentTimeMillis()}
    private fun update(g:Guidance?){
        if(g==null){speech.cancel();if(route && link.ready)send(4,seq);route=false;lastGuidance=null;gate.reset();return}
        if(!link.ready){report("Chờ BLE; chỉ đường chưa được gửi");return}
        if(!route){session=SecureRandom().nextInt().let{if(it==0)1 else it};seq=1;send(1,seq);route=true;gate.reset();lastHeartbeat=0}
        if(g==lastGuidance)return
        lastGuidance=g;seq++;val updateId=seq;val say=gate.accept(g)
        if(say){cue=updateId;generation.incrementAndGet()}
        val token=generation.get();val created=System.currentTimeMillis()
        send(2,updateId,NavProtocol.guidance(g,cue));heartbeat()
        if(say)speech.say(g.instruction,"${session}_$cue") {audio ->
            if(!stopped)io.execute{try{if(generation.get()==token && route)send(5,updateId,audio){generation.get()==token && route && !stopped && System.currentTimeMillis()-created<5000}}catch(e:Exception){report(e.message?:"Lỗi gửi âm thanh")}}
        }
    }
    override fun onStartCommand(intent:Intent?,flags:Int,startId:Int):Int {
        if(intent?.action=="STOP"){stopSelf();return START_NOT_STICKY}
        intent?.getStringExtra("address")?.let{address=it;io.execute{try{link.connect(it);retryAt=System.currentTimeMillis()+15000}catch(e:Exception){report(e.message?:"Không kết nối được")}}}
        return START_NOT_STICKY
    }
    override fun onDestroy(){stopped=true;NavBus.listener=null;generation.incrementAndGet();speech.close();link.close();io.shutdownNow();report("Đã ngắt kết nối");super.onDestroy()}
    override fun onBind(intent:Intent?):IBinder?=null
}
