#include <jni.h>
#include <opus.h>
#include <vector>
#include <algorithm>
extern "C" JNIEXPORT jbyteArray JNICALL Java_dev_minios_companion_PhoneSpeech_encode(JNIEnv *env,jobject,jshortArray input) {
    const jsize n=env->GetArrayLength(input);
    if(n<=0 || n>128000) return nullptr;
    int code;OpusEncoder *encoder=opus_encoder_create(16000,1,OPUS_APPLICATION_VOIP,&code);
    if(!encoder || code!=OPUS_OK) return nullptr;
    if(opus_encoder_ctl(encoder,OPUS_SET_BITRATE(16000))!=OPUS_OK || opus_encoder_ctl(encoder,OPUS_SET_VBR(0))!=OPUS_OK) {opus_encoder_destroy(encoder);return nullptr;}
    std::vector<unsigned char> out;out.reserve(18000);
    short pcm[320];unsigned char packet[1275];bool ok=true;
    for(jsize at=0;at<n;at+=320) {
        std::fill(pcm,pcm+320,0);env->GetShortArrayRegion(input,at,std::min(jsize(320),n-at),pcm);
        if(env->ExceptionCheck()) {ok=false;break;}
        int size=opus_encode(encoder,pcm,320,packet,sizeof(packet));
        if(size<=0 || out.size()+size+2>32768) {ok=false;break;}
        out.push_back(size&255);out.push_back(size>>8);out.insert(out.end(),packet,packet+size);
    }
    opus_encoder_destroy(encoder);
    if(!ok)return nullptr;
    jbyteArray result=env->NewByteArray(out.size());
    if(result)env->SetByteArrayRegion(result,0,out.size(),reinterpret_cast<jbyte*>(out.data()));
    return result;
}
