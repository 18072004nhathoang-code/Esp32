#pragma once
#include <stdint.h>
#include <stdlib.h>
class HomeGesture {
    bool held_=false,edge_=false;
    int x_=0,y_=0,last_x_=0,last_y_=0;
public:
    bool fired=false;
    bool sample(bool pressed,int x,int y) {
        fired=false;
        if(pressed) {
            if(!held_) {x_=x;y_=y;edge_=y>=304;}
            held_=true;last_x_=x;last_y_=y;return edge_;
        }
        fired=held_ && edge_ && y_-last_y_>=32 && abs(last_x_-x_)<=48;
        held_=edge_=false;return false;
    }
    void reset() {held_=edge_=fired=false;}
};
