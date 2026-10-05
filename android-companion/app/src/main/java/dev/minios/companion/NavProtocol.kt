package dev.minios.companion

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Locale

data class Guidance(val maneuver:Int,val meters:Long?,val instruction:String,val road:String="",val eta:String="")
object NavProtocol {
    fun clip(s:String,bytes:Int):ByteArray {
        val b=StringBuilder(); var n=0
        s.codePoints().forEach { c -> val v=String(Character.toChars(c));val size=v.toByteArray(Charsets.UTF_8).size
            if(n+size<=bytes && c>=32) {b.append(v);n+=size} else n=bytes+1 }
        return b.toString().toByteArray(Charsets.UTF_8)
    }
    fun guidance(g:Guidance,cue:Int=1):ByteArray {
        val text=clip(g.instruction,320);val road=clip(g.road,96);val eta=clip(g.eta,48)
        require(text.isNotEmpty());return ByteBuffer.allocate(13+text.size+road.size+eta.size).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(cue).put(g.maneuver.toByte()).putInt(g.meters?.toInt()?:-1).putShort(text.size.toShort()).put(road.size.toByte()).put(eta.size.toByte()).put(text).put(road).put(eta).array()
    }
    fun fragments(kind:Int,session:Int,seq:Int,body:ByteArray,mtu:Int):List<ByteArray> {
        require(session!=0 && seq>0 && kind in 1..5 && body.size<=if(kind==5)32768 else 480)
        require(mtu>=23);val amount=minOf(244,mtu-3)-14
        val parts=if(body.isEmpty())listOf(ByteArray(0)) else body.toList().chunked(amount).map{it.toByteArray()}
        var offset=0
        return parts.map { part -> ByteBuffer.allocate(14+part.size).order(ByteOrder.LITTLE_ENDIAN).put(1).put(kind.toByte()).putInt(session).putInt(seq)
            .putShort(body.size.toShort()).putShort(offset.toShort()).put(part).array().also{offset+=part.size} }
    }
}
object MapsParser {
    fun parse(packageName:String,ongoing:Boolean,title:String,text:String,detail:String=""):Guidance? {
        if(packageName!="com.google.android.apps.maps" || !ongoing) return null
        val raw=listOf(title,text).filter{it.isNotBlank()}.distinct().joinToString(" · ").replace(Regex("[\\r\\n\\t]+")," ")
        if(raw.isBlank()) return null
        val s=raw.lowercase(Locale.ROOT)
        val maneuver=when {
            Regex("đã đến|bạn đã tới|you have arrived|you've arrived").containsMatchIn(s)->5
            Regex("quay đầu|u.turn").containsMatchIn(s)->4
            Regex("vòng xuyến|roundabout").containsMatchIn(s)->6
            Regex("rẽ trái|rẽ sang trái|turn left|slight left|keep left").containsMatchIn(s)->2
            Regex("rẽ phải|rẽ sang phải|turn right|slight right|keep right").containsMatchIn(s)->3
            Regex("đi thẳng|tiếp tục đi|continue straight|head straight").containsMatchIn(s)->1
            else->0
        }
        val match=Regex("(?<![\\d:])(\\d+(?:[.,]\\d+)?)\\s*(km|m)\\b").find(s)
        val meters=match?.let{(it.groupValues[1].replace(',','.').toDouble()*(if(it.groupValues[2]=="km")1000 else 1)).toLong()}
        // Preserve source text. Never infer a street/ETA from ambiguous notification fields.
        val eta=detail.takeIf{Regex("\\d.*(min|phút|giờ|ETA|đến)",RegexOption.IGNORE_CASE).containsMatchIn(it)}?:""
        return Guidance(maneuver,meters,raw,eta=eta)
    }
}
class SpeechGate {
    private var key="";private var bucket=-1
    fun accept(g:Guidance):Boolean {
        val current="${g.maneuver}:"+g.instruction.replace(Regex("\\d+(?:[.,]\\d+)?\\s*(km|m)\\b"),"")
        val distance=g.meters?:Long.MAX_VALUE
        val b=when {distance<=50->0;distance<=100->1;distance<=200->2;distance<=500->3;else->4}
        val say=current!=key || b<bucket
        key=current;bucket=b;return say
    }
    fun reset(){key="";bucket=-1}
}
