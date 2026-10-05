#pragma once
#include "ui_shell.h"
#include "../connectivity/navigation_service.h"
namespace navigation_ui {
struct View {lv_obj_t *root=nullptr,*arrow=nullptr,*distance=nullptr,*text=nullptr,*detail=nullptr;uint8_t maneuver=0;};
inline void draw(lv_event_t *e) {
    View *v=static_cast<View*>(lv_event_get_user_data(e));
    if(!v || v->maneuver==0) return;
    lv_draw_ctx_t *ctx=lv_event_get_draw_ctx(e);
    lv_area_t area;lv_obj_get_coords(v->arrow,&area);
    lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);d.color=lv_color_hex(COLOR_ACCENT_BLUE);d.width=8;d.round_start=d.round_end=1;
    lv_point_t pts[4]={{40,66},{40,10},{20,30},{40,10}};
    auto transform=[&](lv_point_t p) {
        if(v->maneuver==2) p={p.y,static_cast<lv_coord_t>(80-p.x)};
        if(v->maneuver==3) p={static_cast<lv_coord_t>(80-p.y),p.x};
        p.x+=area.x1;p.y+=area.y1;return p;
    };
    if(v->maneuver==4) {pts[0]={60,66};pts[1]={60,18};pts[2]={22,18};pts[3]={22,58};}
    if(v->maneuver==5) {pts[0]={12,40};pts[1]={32,60};pts[2]={66,18};pts[3]={66,18};}
    if(v->maneuver==6) {pts[0]={40,66};pts[1]={18,40};pts[2]={40,14};pts[3]={65,40};}
    for(int i=0;i<3;++i) {auto a=transform(pts[i]),b=transform(pts[i+1]);lv_draw_line(ctx,&d,&a,&b);}
    if(v->maneuver<=3) {auto a=transform({40,10}),b=transform({60,30});lv_draw_line(ctx,&d,&a,&b);}
}
inline lv_obj_t *label(lv_obj_t *root,int y,int h,const lv_font_t *font) {
    lv_obj_t *o=lv_label_create(root);lv_obj_set_size(o,216,h);lv_obj_set_pos(o,12,y);
    lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);
    lv_label_set_long_mode(o,LV_LABEL_LONG_DOT);return o;
}
inline void open(View &v,lv_obj_t *parent,lv_event_cb_t toggle) {
    v=View{};v.root=lv_obj_create(parent);minios_shell::flat(v.root,COLOR_OS_BG);lv_obj_set_size(v.root,240,APP_CONTENT_HEIGHT);
    v.arrow=lv_obj_create(v.root);minios_shell::flat(v.arrow,COLOR_OS_BG);lv_obj_set_size(v.arrow,80,80);lv_obj_set_pos(v.arrow,80,0);
    lv_obj_add_event_cb(v.arrow,draw,LV_EVENT_DRAW_MAIN,&v);
    v.distance=label(v.root,80,22,UI_FONT_16);v.text=label(v.root,108,56,UI_FONT_14);v.detail=label(v.root,167,23,UI_FONT_12);
    lv_obj_t *button=lv_btn_create(v.root);minios_shell::flat(button,COLOR_CARD_BG,14);lv_obj_set_size(button,110,44);lv_obj_set_pos(button,65,APP_CONTENT_HEIGHT-44);
    lv_obj_add_event_cb(button,toggle,LV_EVENT_CLICKED,nullptr);lv_obj_t *l=lv_label_create(button);lv_label_set_text(l,"Bản đồ");lv_obj_center(l);
}
inline void update(View &v,const NavigationSnapshot &s) {
    if(!v.root) return;
    v.maneuver=s.fresh?s.guidance.maneuver:0;lv_obj_invalidate(v.arrow);
    char distance[40];
    if(!s.active) snprintf(distance,sizeof(distance),"Chưa dẫn đường");
    else if(!s.fresh) snprintf(distance,sizeof(distance),"Mất kết nối");
    else if(s.guidance.meters!=UINT32_MAX) snprintf(distance,sizeof(distance),"%lu m",static_cast<unsigned long>(s.guidance.meters));
    else snprintf(distance,sizeof(distance),"%s",s.guidance.maneuver==5?"Đã đến nơi":"Chỉ đường");
    lv_label_set_text(v.distance,distance);
    lv_label_set_text(v.text,s.fresh?s.guidance.instruction:"Mở Google Maps trên điện thoại và bắt đầu dẫn đường.");
    lv_label_set_text_fmt(v.detail,"%s%s%s",s.guidance.road,s.guidance.road[0]&&s.guidance.eta[0]?" · ":"",s.guidance.eta);
}
}
