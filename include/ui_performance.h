#pragma once
#include <stdint.h>
#include <stddef.h>
namespace ui_performance {
struct Histogram {
    uint32_t buckets[101]={};uint32_t count=0,max_us=0,over_200ms=0;
    void add(uint32_t us) {size_t b=(us+1999)/2000;if(b>100)b=100;++buckets[b];++count;if(us>max_us)max_us=us;if(us>200000)++over_200ms;}
    uint32_t p95_ms() const {if(!count)return 0;uint32_t target=(count*95+99)/100,sum=0;for(unsigned i=0;i<101;++i){sum+=buckets[i];if(sum>=target)return i*2;}return 200;}
};
}
