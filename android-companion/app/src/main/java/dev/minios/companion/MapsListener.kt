package dev.minios.companion
import android.app.Notification
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification

object NavBus {
    @Volatile var state="Chưa kết nối"
    @Volatile var guidance:Guidance?=null
    @Volatile var listener:((Guidance?)->Unit)?=null
}
class MapsListener:NotificationListenerService(){
    private var activeKey:String?=null
    override fun onListenerConnected(){activeNotifications?.forEach{onNotificationPosted(it)}}
    override fun onNotificationPosted(s:StatusBarNotification){
        val n=s.notification
        if(n.flags and Notification.FLAG_GROUP_SUMMARY!=0)return
        val e=n.extras
        val contents=listOf(Notification.EXTRA_TITLE,Notification.EXTRA_TEXT).joinToString(" "){e.getCharSequence(it)?.toString()?:""}
        if(n.category!="navigation" && !Regex("rẽ|đi thẳng|tiếp tục|quay đầu|vòng xuyến|đã đến|turn |continue |roundabout|arrived|\\d+\\s*(m|km)\\b",RegexOption.IGNORE_CASE).containsMatchIn(contents))return
        val g=MapsParser.parse(s.packageName,s.isOngoing,e.getCharSequence(Notification.EXTRA_TITLE)?.toString()?:"",
            e.getCharSequence(Notification.EXTRA_TEXT)?.toString()?:"",e.getCharSequence(Notification.EXTRA_SUB_TEXT)?.toString()?:"")?:return
        activeKey=s.key;NavBus.guidance=g;NavBus.listener?.invoke(g)
    }
    override fun onNotificationRemoved(s:StatusBarNotification){
        if(s.key!=activeKey)return
        activeKey=null;NavBus.guidance=null;NavBus.listener?.invoke(null)
    }
    override fun onListenerDisconnected(){NavBus.state="Quyền đọc thông báo bị ngắt";NavBus.listener?.invoke(null)}
}
