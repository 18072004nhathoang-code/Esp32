package dev.minios.companion

import android.content.Context
import android.media.AudioFormat
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.Locale
import java.util.concurrent.Executors

class PhoneSpeech(private val context:Context,private val report:(String)->Unit) {
    companion object { init { System.loadLibrary("minios_opus") } }
    private external fun encode(pcm:ShortArray):ByteArray?
    private val executor=Executors.newSingleThreadExecutor()
    private var tts:TextToSpeech?=null
    @Volatile private var available=false
    private data class Pending(val id:String,val done:(ByteArray)->Unit,val data:ByteArrayOutputStream=ByteArrayOutputStream(),var rate:Int=0,var channels:Int=0,var valid:Boolean=true)
    private var pending:Pending?=null
    init {
        tts=TextToSpeech(context) { status ->
            val engine=tts
            if(status==TextToSpeech.SUCCESS && engine!=null) {
                val voice=engine.voices?.filter{it.locale.language=="vi" && !it.isNetworkConnectionRequired}?.sortedBy{it.name}?.firstOrNull()
                if(voice!=null){engine.voice=voice;available=true;report("Giọng tiếng Việt offline sẵn sàng")}
                else report("Cần cài giọng tiếng Việt offline trong Cài đặt chuyển văn bản thành giọng nói")
            } else report("Không khởi tạo được TTS Android")
        }
        tts?.setOnUtteranceProgressListener(object:UtteranceProgressListener(){
            override fun onStart(id:String){}
            override fun onBeginSynthesis(id:String,rate:Int,format:Int,channels:Int) = synchronized(this@PhoneSpeech) {
                pending?.takeIf{it.id==id}?.apply{this.rate=rate;this.channels=channels;valid=format==AudioFormat.ENCODING_PCM_16BIT && channels in 1..2 && rate in 8000..48000};Unit
            }
            override fun onAudioAvailable(id:String,audio:ByteArray) = synchronized(this@PhoneSpeech) {
                pending?.takeIf{it.id==id}?.apply{if(data.size()+audio.size>rate*channels*2*8)valid=false else if(valid)data.write(audio)};Unit
            }
            override fun onDone(id:String) {
                val item=synchronized(this@PhoneSpeech){pending?.takeIf{it.id==id}.also{if(it!=null)pending=null}}
                File(context.cacheDir,"nav-$id.wav").delete()
                if(item==null)return
                executor.execute {
                    try {
                        check(item.valid && item.rate>0 && item.data.size()>0){"TTS quá 8 giây hoặc định dạng không hỗ trợ"}
                        val bytes=item.data.toByteArray();check(bytes.size%(2*item.channels)==0)
                        val samples=bytes.size/(2*item.channels)
                        val mono=DoubleArray(samples){i -> (0 until item.channels).sumOf{c ->
                            val at=(i*item.channels+c)*2;((bytes[at].toInt() and 255) or (bytes[at+1].toInt() shl 8)).toShort().toDouble()
                        }/item.channels}
                        // Windowed-sinc resampler includes anti-alias filtering for rates above 16kHz.
                        val output=ShortArray((samples.toLong()*16000/item.rate).toInt()) {i ->
                            val position=i*item.rate/16000.0;val cutoff=minOf(1.0,16000.0/item.rate)*0.9
                            var value=0.0;var weight=0.0
                            for(k in -16..16){val index=position.toInt()+k;if(index !in mono.indices)continue
                                val d=position-index;val x=Math.PI*d*cutoff
                                val w=(if(kotlin.math.abs(x)<1e-8)1.0 else kotlin.math.sin(x)/x)*(0.5+0.5*kotlin.math.cos(Math.PI*d/17))
                                value+=mono[index]*w;weight+=w}
                            (value/weight).toInt().coerceIn(-32768,32767).toShort()
                        }
                        val encoded=encode(output)?:error("Không mã hóa được Opus")
                        item.done(encoded)
                    }catch(e:Exception){report(e.message?:"TTS lỗi")}
                }
            }
            @Deprecated("API callback") override fun onError(id:String) {synchronized(this@PhoneSpeech){if(pending?.id==id)pending=null};File(context.cacheDir,"nav-$id.wav").delete();report("TTS không tạo được lời đọc")}
        })
    }
    @Synchronized fun say(text:String,id:String,done:(ByteArray)->Unit) {
        cancel();if(!available){report("Chưa có giọng tiếng Việt offline; chỉ hiển thị hướng dẫn");return}
        pending=Pending(id,done)
        if(tts?.synthesizeToFile(text,null,File(context.cacheDir,"nav-$id.wav"),id)!=TextToSpeech.SUCCESS){pending=null;report("TTS từ chối yêu cầu")}
    }
    @Synchronized fun cancel(){tts?.stop();pending?.let{File(context.cacheDir,"nav-${it.id}.wav").delete()};pending=null}
    fun close(){cancel();tts?.shutdown();executor.shutdownNow()}
}
