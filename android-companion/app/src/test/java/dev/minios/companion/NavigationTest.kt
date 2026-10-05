package dev.minios.companion
import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class NavigationTest {
    // Synthetic fixtures. Real-phone notification samples remain a hardware acceptance gate.
    @Test fun parser(){
        val g=MapsParser.parse("com.google.android.apps.maps",true,"200 m","Rẽ trái vào Nguyễn Văn Linh")!!
        assertEquals(2,g.maneuver);assertEquals(200L,g.meters);assertTrue(g.instruction.contains("Nguyễn"))
        assertNull(MapsParser.parse("other.app",true,"200 m","Turn left"))
        assertNull(MapsParser.parse("com.google.android.apps.maps",false,"200 m","Turn left"))
        assertEquals(3,MapsParser.parse("com.google.android.apps.maps",true,"1.2 km","Turn right")!!.maneuver)
        assertEquals(1200L,MapsParser.parse("com.google.android.apps.maps",true,"1,2 km","Turn right")!!.meters)
        assertEquals(0,MapsParser.parse("com.google.android.apps.maps",true,"Thông báo lạ","Chỉ dẫn mới")!!.maneuver)
        assertEquals(5,MapsParser.parse("com.google.android.apps.maps",true,"Bạn đã đến","Điểm đến")!!.maneuver)
    }
    @Test fun protocol(){
        val body=NavProtocol.guidance(Guidance(2,200,"Rẽ trái"),7)
        val b=ByteBuffer.wrap(body).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(7,b.int);assertEquals(2,b.get().toInt());assertEquals(200,b.int)
        val tiny=NavProtocol.fragments(2,123,2,body,23);assertTrue(tiny.size>1)
        assertTrue(tiny.all{it.size<=20})
        val joined=tiny.flatMap{it.drop(14)}.toByteArray();assertArrayEquals(body,joined)
        val big=NavProtocol.fragments(5,123,3,ByteArray(32768),247);assertTrue(big.all{it.size<=244})
        val text="Đường Việt Nam 🇻🇳".repeat(100)
        val clipped=NavProtocol.clip(text,320);assertTrue(clipped.size<=320);assertFalse(String(clipped,Charsets.UTF_8).contains('\uFFFD'))
    }
    @Test fun speechDedup(){
        val gate=SpeechGate()
        assertTrue(gate.accept(Guidance(2,800,"800 m · Rẽ trái")))
        assertFalse(gate.accept(Guidance(2,700,"700 m · Rẽ trái")))
        assertTrue(gate.accept(Guidance(2,500,"500 m · Rẽ trái")))
        assertFalse(gate.accept(Guidance(2,450,"450 m · Rẽ trái")))
        assertTrue(gate.accept(Guidance(3,450,"450 m · Rẽ phải")))
        gate.reset();assertTrue(gate.accept(Guidance(3,450,"450 m · Rẽ phải")))
    }
}
