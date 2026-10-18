/* SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include "ui_components.hpp"
#include "wallpaper.hpp"
#include "aera_fallback.hpp"
#include "browser/session.hpp"

namespace aeraui {
namespace {
using namespace design;
using namespace widgets;
struct LockState {
  lv_obj_t *root{}, *background{}, *content{}, *group{}, *clock{}, *date{}, *owner{}, *info{};
  lv_obj_t *slider{}, *knob{}, *hint{}, *hold_progress{};
  lv_timer_t *timer{};
  ActionCallback callback{}; void *context{};
  LockTheme theme;
  int w{}, h{}, travel{}, offset{}, origin{}, origin_y{};
  uint32_t pressed_at{}, last_telemetry{};
  bool dragging{}, unlocking{}, preview{}, landscape{};
  bool ambient{};
  int dial_diameter = -1, date_day = -1, date_year = -1;
  std::string time;
  struct Stroke {lv_obj_t *object{};lv_point_precise_t points[2]{};};
  std::array<Stroke,80> strokes{};
  lv_obj_t *hours{}, *minutes{};
};
int Px(LockState *s, int value) { return std::max(1, value * std::min(s->w, s->h) / 1440); }
void Passive(lv_obj_t *object) {
  lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
  for (uint32_t i=0;i<lv_obj_get_child_count(object);++i) Passive(lv_obj_get_child(object,i));
}
lv_obj_t *Text(LockState *s, lv_obj_t *parent, const char *text, int size, lv_color_t color) {
  auto *label = Label(parent, text, fonts::DisplayFont(Px(s,size)), color);
  lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
  return label;
}
long ReadNumber(const char *name) {
  std::ifstream file(std::string("/sys/class/power_supply/battery/") + name);
  long value = -1; file >> value; return value;
}
void Stroke(LockState *s,int index,int ax,int ay,int bx,int by,int width,lv_color_t color) {
  auto &stroke=s->strokes[index];
  const bool created = !stroke.object;
  const bool moved = created || stroke.points[0].x != ax || stroke.points[0].y != ay ||
      stroke.points[1].x != bx || stroke.points[1].y != by;
  if(!stroke.object) {
    stroke.object=lv_line_create(s->clock);
    lv_obj_remove_flag(stroke.object,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_rounded(stroke.object,true,0);
  }
  if(moved) {
    // Invalidate the old bounds before changing the retained point array.
    lv_obj_invalidate(stroke.object);
    stroke.points[0]={lv_value_precise_t(ax),lv_value_precise_t(ay)};
    stroke.points[1]={lv_value_precise_t(bx),lv_value_precise_t(by)};
    lv_line_set_points(stroke.object,stroke.points,2);
  }
  if(created || lv_obj_get_style_line_width(stroke.object,LV_PART_MAIN)!=width)
    lv_obj_set_style_line_width(stroke.object,width,0);
  if(created || !lv_color_eq(lv_obj_get_style_line_color(stroke.object,LV_PART_MAIN),color))
    lv_obj_set_style_line_color(stroke.object,color,0);
  lv_obj_remove_flag(stroke.object,LV_OBJ_FLAG_HIDDEN);
}
void DigitalClock(LockState *s) {
  const int h = lv_obj_get_height(s->clock), digit = h*55/100, stroke = std::max(2,h/14);
  const int gap = h/9;
  const int width = digit*4+gap*4+stroke;
  int x = (lv_obj_get_width(s->clock)-width)/2;
  static const uint8_t masks[] = {0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f};
  const lv_color_t color = s->theme.colour == 0 ? kAccent :
      s->theme.colour == 2 ? Color(s->theme.rgb) : lv_color_white();
  int index=0;
  auto segment = [&](int ax,int ay,int bx,int by) {Stroke(s,index++,ax,ay,bx,by,stroke,color);};
  for (char c : s->time) {
    if (c == ':') {
      segment(x,h/3,x,h/3+1);
      segment(x,h*2/3,x,h*2/3+1); x+=stroke+gap; continue;
    }
    if (c<'0'||c>'9') continue;
    const int y=stroke, mid=h/2, bottom=h-1-stroke;
    const int l=x+stroke, r=x+digit-stroke;
    const uint8_t m=masks[c-'0'];
    if(m&1) segment(l,y,r,y);
    if(m&2) segment(r,y,r,mid);
    if(m&4) segment(r,mid,r,bottom);
    if(m&8) segment(l,bottom,r,bottom);
    if(m&16) segment(l,mid,l,bottom);
    if(m&32) segment(l,y,l,mid);
    if(m&64) segment(l,mid,r,mid);
    x+=digit+gap;
  }
  for(size_t i=index;i<s->strokes.size();++i)
    if(s->strokes[i].object)lv_obj_add_flag(s->strokes[i].object,LV_OBJ_FLAG_HIDDEN);
}
void AnalogueClock(LockState *s,const tm &local,double fraction) {
  const int d=lv_obj_get_width(s->clock),c=d/2;
  const auto color=s->theme.colour==0?kAccent:s->theme.colour==2?Color(s->theme.rgb):lv_color_white();
  auto radial=[&](int index,double turn,double inner,double outer,int width,lv_color_t ink) {
    const double angle=turn*6.283185307179586-1.5707963267948966;
    Stroke(s,index,c+int(std::cos(angle)*inner),c+int(std::sin(angle)*inner),
        c+int(std::cos(angle)*outer),c+int(std::sin(angle)*outer),width,ink);
  };
  // Theme changes recreate the scene. The dial is static until its size changes.
  if(s->dial_diameter!=d) {
    for(int i=0;i<60;++i)radial(i,i/60.0,d*(i%5==0?.40:.44),d*.47,std::max(1,d/(i%5==0?100:220)),color);
    s->dial_diameter=d;
  }
  const double seconds=local.tm_sec+(s->ambient?0.0:fraction);
  radial(60,(local.tm_hour%12+local.tm_min/60.0)/12,0,d*.25,std::max(2,d/45),color);
  radial(61,(local.tm_min+seconds/60)/60,0,d*.36,std::max(2,d/65),color);
  radial(62,seconds/60,-d*.08,d*.39,std::max(1,d/180),kAccent);
}
void Refresh(LockState *s) {
  // Use one wall-clock sample for both whole and fractional seconds, including
  // at minute rollover. Ambient ticks must land on the dial's second marks.
  timespec now{};clock_gettime(CLOCK_REALTIME,&now);
  tm local{};localtime_r(&now.tv_sec,&local);
  if(s->ambient && s->timer)
    lv_timer_set_period(s->timer,1000-static_cast<uint32_t>(now.tv_nsec/1000000));
  const bool clock24=RecoveryPreference(Preference::kClock24);
  const int hour=clock24 ? local.tm_hour : (local.tm_hour%12 ? local.tm_hour%12 : 12);
  char clock[32]; snprintf(clock,sizeof(clock),s->theme.clock==2 ? "%02d\n%02d" : "%02d:%02d",hour,local.tm_min);
  const bool time_changed=s->time!=clock;
  s->time=clock;
  if(s->theme.clock==4) DigitalClock(s);
  else if(s->theme.clock==1) AnalogueClock(s,local,now.tv_nsec/1e9);
  else if(s->theme.clock==3 && time_changed) {
    lv_label_set_text_fmt(s->hours,"%02d",hour);
    lv_label_set_text_fmt(s->minutes,"%02d",local.tm_min);
  }
  else if(s->theme.clock!=3 && time_changed) lv_label_set_text(s->clock,clock);
  if(s->date_day!=local.tm_yday || s->date_year!=local.tm_year) {
  s->date_day=local.tm_yday;s->date_year=local.tm_year;
  static const char *days[]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  static const char *months[]={"January","February","March","April","May","June","July","August","September","October","November","December"};
  auto date=i18n::Format("%s, %d %s",i18n::Translate(days[local.tm_wday]),local.tm_mday,i18n::Translate(months[local.tm_mon]));
  if(s->theme.date_style==1) date=i18n::Format("%d %s",local.tm_mday,i18n::Translate(months[local.tm_mon]));
  if(s->theme.date_style==2) {char numeric[24];snprintf(numeric,sizeof(numeric),"%04d-%02d-%02d",local.tm_year+1900,local.tm_mon+1,local.tm_mday);date=numeric;}
  if(s->theme.date_style==3)date=i18n::Format("%s\n%d %s",i18n::Translate(days[local.tm_wday]),local.tm_mday,i18n::Translate(months[local.tm_mon]));
  lv_label_set_text(s->date,date.c_str());
  }
  if(s->ambient) {
    lv_obj_set_style_translate_x(s->group,Px(s,12)*(local.tm_min%3-1),0);
    lv_obj_set_style_translate_y(s->group,Px(s,12)*((local.tm_min/3)%3-1),0);
    if(s->theme.date_placement==2) {
      lv_obj_set_style_translate_x(s->date,Px(s,12)*(local.tm_min%3-1),0);
      lv_obj_set_style_translate_y(s->date,Px(s,12)*((local.tm_min/3)%3-1),0);
    }
  }
}
void SetOffset(LockState *s,int offset) {
  s->offset=std::clamp(offset,0,s->travel);
  if(s->knob) lv_obj_set_x(s->knob,s->offset);
}
void SwipeFeedback(LockState *s,int distance,bool animate=false) {
  auto move=[&](lv_obj_t *object) {
    lv_anim_delete(object,nullptr);
    if(!animate){lv_obj_set_style_translate_y(object,-distance,0);return;}
    lv_anim_t a;lv_anim_init(&a);lv_anim_set_var(&a,object);
    lv_anim_set_values(&a,lv_obj_get_style_translate_y(object,LV_PART_MAIN),-distance);
    lv_anim_set_duration(&a,160);lv_anim_set_path_cb(&a,lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a,[](void *o,int32_t v){lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(o),v,0);});
    lv_anim_start(&a);
  };
  move(s->group);move(s->slider);
  if(s->theme.date_placement==2)move(s->date);
}
void HoldFeedback(LockState *s,int progress) {
  if(!s->hold_progress)return;
  if(progress<0){lv_obj_add_flag(s->hold_progress,LV_OBJ_FLAG_HIDDEN);return;}
  lv_obj_remove_flag(s->hold_progress,LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_width(s->hold_progress,std::max(1,(lv_obj_get_width(s->slider)-Px(s,80))*std::min(progress,100)/100));
}
void Unlock(LockState *s) {
  if(s->unlocking||s->preview) return;
  s->unlocking=true;
  const bool swipe=s->theme.unlock==1;
  if(swipe) {
    lv_anim_t a;lv_anim_init(&a);lv_anim_set_var(&a,s->root);
    lv_anim_set_values(&a,0,-s->h);lv_anim_set_duration(&a,240);
    lv_anim_set_path_cb(&a,lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a,[](void *o,int32_t v){lv_obj_set_y(static_cast<lv_obj_t *>(o),v);});
    lv_anim_start(&a);
  } else if(s->theme.animation==1) lv_obj_fade_out(s->root,200,0);
  auto *timer=lv_timer_create([](lv_timer_t *t){
    auto *s=static_cast<LockState *>(lv_timer_get_user_data(t));
    s->timer=nullptr;
    if(s->callback) s->callback(Action::kUnlock,s->context);
  },swipe?250:s->theme.animation?220:1,s);
  lv_timer_set_repeat_count(timer,1);
  if(s->timer) lv_timer_delete(s->timer);
  s->timer=timer;
}
void Touch(lv_event_t *event) {
  auto *s=static_cast<LockState *>(lv_event_get_user_data(event));
  if(s->preview||s->unlocking) return;
  auto *input=lv_indev_active(); if(!input) return;
  lv_point_t p;lv_indev_get_point(input,&p);
  const auto code=lv_event_get_code(event);
  if(code==LV_EVENT_PRESSED) {
    s->origin=p.x;s->origin_y=p.y;s->pressed_at=lv_tick_get();s->dragging=true;
    if(s->theme.unlock==1)SwipeFeedback(s,0);
    if(s->theme.unlock==2)HoldFeedback(s,0);
  } else if(code==LV_EVENT_PRESSING && s->dragging) {
    if(s->theme.unlock==0) SetOffset(s,p.x-s->origin);
    if(s->theme.unlock==1)SwipeFeedback(s,std::clamp(s->origin_y-p.y,0,s->h/2));
    if(s->theme.unlock==2) {
      lv_area_t area;lv_obj_get_coords(s->slider,&area);
      if(p.x<area.x1||p.x>area.x2||p.y<area.y1||p.y>area.y2) {s->dragging=false;HoldFeedback(s,-1);}
      else {
        const auto elapsed=lv_tick_elaps(s->pressed_at);
        HoldFeedback(s,std::min<uint32_t>(100,elapsed*100/900));
        if(elapsed>=900)Unlock(s);
      }
    }
  } else if(code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST) {
    const bool accepted=s->dragging && code==LV_EVENT_RELEASED &&
      ((s->theme.unlock==0 && s->offset>=s->travel*75/100) ||
       (s->theme.unlock==1 && s->origin_y-p.y>Px(s,260) && std::abs(p.x-s->origin)<Px(s,300)));
    s->dragging=false;
    if(accepted) Unlock(s); else {
      SetOffset(s,0);HoldFeedback(s,-1);
      if(s->theme.unlock==1)SwipeFeedback(s,0,true);
    }
  }
}
}
lv_obj_t *BuildLockScene(lv_obj_t *parent,ActionCallback callback,void *context,
                         bool preview,const LockTheme *theme,bool ambient) {
  auto *s=new LockState;
  s->theme=theme?*theme:RecoveryLockTheme();s->callback=callback;s->context=context;s->preview=preview;
  s->ambient=ambient;
  // Ignore retired information/shortcut options in previously saved themes.
  s->theme.charging=s->theme.battery=s->theme.wifi=s->theme.activity=false;
  s->theme.flashlight=s->theme.screenshot=s->theme.minimal=false;
  s->w=lv_obj_get_width(parent);s->h=lv_obj_get_height(parent);s->landscape=s->w>s->h;
  auto *root=lv_obj_create(parent);s->root=root; Clear(root);
  lv_obj_set_size(root,s->w,s->h);lv_obj_set_pos(root,0,0);
  const int radius=preview?lv_obj_get_style_radius(parent,LV_PART_MAIN):0;
  lv_obj_set_style_radius(root,radius,0);
  lv_obj_set_style_clip_corner(root,true,0);
  lv_obj_add_event_cb(root,[](lv_event_t *e){
    auto *s=static_cast<LockState *>(lv_event_get_user_data(e));
    if(s->timer)lv_timer_delete(s->timer);delete s;
  },LV_EVENT_DELETE,s);
  auto *bg=lv_obj_create(root);s->background=bg;Clear(bg);lv_obj_set_size(bg,LV_PCT(100),LV_PCT(100));
  MainBackground(bg);
  lv_obj_set_style_radius(bg,radius,0);
  lv_obj_set_style_clip_corner(bg,true,0);
  // Replace only the background layer; foreground labels never enter the blur.
  if(!s->theme.wallpaper.empty()||s->theme.blur||(preview&&wallpaper::Active())) {
    lv_obj_clean(bg);
    lv_obj_set_style_bg_color(bg,kMainCanvas,0);lv_obj_set_style_bg_opa(bg,LV_OPA_COVER,0);
    wallpaper::AttachLock(bg,s->theme.wallpaper,s->theme.blur);
  }
  auto *dim=lv_obj_create(bg);Clear(dim);lv_obj_set_size(dim,LV_PCT(100),LV_PCT(100));
  lv_obj_set_style_radius(dim,radius,0);
  lv_obj_set_style_bg_color(dim,lv_color_black(),0);
  lv_obj_set_style_bg_opa(dim,s->theme.dim*255/100,0);lv_obj_remove_flag(dim,LV_OBJ_FLAG_CLICKABLE);
  if(ambient) {
    lv_obj_clean(bg);
    lv_obj_set_style_bg_image_src(bg,nullptr,0);
    lv_obj_set_style_bg_color(bg,lv_color_black(),0);
    lv_obj_set_style_bg_opa(bg,LV_OPA_COVER,0);
  }
  int status=preview?Px(s,141):StatusBarHeight();
  if(!s->theme.minimal && !ambient) {
    if(preview) {
      auto *top=Text(s,root,"09:41",30,lv_color_white());lv_obj_set_pos(top,Px(s,64),Px(s,45));
      auto *battery=Text(s,root,"100%",28,lv_color_white());lv_obj_align(battery,LV_ALIGN_TOP_RIGHT,-Px(s,64),Px(s,45));
    } else AttachStatusBar(root,nullptr,nullptr,StatusBarAction::kNone,true);
  }
  auto *content=lv_obj_create(root);s->content=content;Clear(content);
  lv_obj_set_pos(content,0,status);lv_obj_set_size(content,s->w,s->h-status);
  if(ambient)lv_obj_set_style_opa(content,LV_OPA_40,0);
  auto *group=lv_obj_create(content);s->group=group;Clear(group);
  lv_obj_set_width(group,s->w-Px(s,120));lv_obj_set_height(group,LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(group,LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(group,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(group,Px(s,22),0);
  const auto color=s->theme.colour==0?kAccent:s->theme.colour==2?Color(s->theme.rgb):lv_color_white();
  const int size=240*s->theme.clock_size/100;
  if(s->theme.clock==4 || s->theme.clock==1) {
    s->clock=lv_obj_create(group);Clear(s->clock);
    const int diameter=Px(s,620*s->theme.clock_size/100);
    lv_obj_set_size(s->clock,s->theme.clock==1?diameter:Px(s,1100),
        s->theme.clock==1?diameter:Px(s,230*s->theme.clock_size/100));
  } else if(s->theme.clock==3) {
    s->clock=lv_obj_create(group);Clear(s->clock);
    const int tile_w=Px(s,320*s->theme.clock_size/100),tile_h=Px(s,330*s->theme.clock_size/100),gap=Px(s,20);
    lv_obj_set_size(s->clock,tile_w*2+gap,tile_h);
    for(int i=0;i<2;++i) {
      auto *tile=lv_obj_create(s->clock);Clear(tile);
      lv_obj_set_size(tile,tile_w,tile_h);lv_obj_set_x(tile,i*(tile_w+gap));
      lv_obj_set_style_radius(tile,Px(s,24),0);
      lv_obj_set_style_bg_color(tile,Color(0x202328),0);lv_obj_set_style_bg_opa(tile,LV_OPA_COVER,0);
      auto *label=Text(s,tile,"00",size,color);lv_obj_center(label);
      if(i==0)s->hours=label;else s->minutes=label;
      auto *seam=lv_obj_create(tile);Clear(seam);lv_obj_set_size(seam,tile_w, std::max(1,Px(s,3)));
      lv_obj_center(seam);lv_obj_set_style_bg_color(seam,lv_color_black(),0);lv_obj_set_style_bg_opa(seam,LV_OPA_70,0);
    }
  } else {
    s->clock=Text(s,group,"00:00",s->landscape?size*3/4:size,color);
    lv_obj_set_style_text_align(s->clock,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_line_space(s->clock,-Px(s,25),0);
  }
  s->date=Text(s,group,"",40*s->theme.date_size/100,s->theme.date_colour==2?kAccent:s->theme.date_colour==1?color:lv_color_white());
  if(s->theme.date_style==4)lv_obj_add_flag(s->date,LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_width(s->date,LV_PCT(100));lv_obj_set_style_text_align(s->date,LV_TEXT_ALIGN_CENTER,0);
  if(s->theme.date_placement==1)lv_obj_move_to_index(s->date,0);
  if(s->theme.date_placement==2) {
    lv_obj_set_parent(s->date,content);
    lv_obj_set_width(s->date,s->w-Px(s,120));
    lv_obj_align(s->date,LV_ALIGN_BOTTOM_MID,0,-Px(s,s->landscape?260:390));
  }
  s->owner=Text(s,group,s->theme.owner.c_str(),30,lv_color_white());
  lv_obj_set_width(s->owner,LV_PCT(90));lv_obj_set_style_text_align(s->owner,LV_TEXT_ALIGN_CENTER,0);
  if(s->theme.owner.empty()||s->theme.minimal||ambient)lv_obj_add_flag(s->owner,LV_OBJ_FLAG_HIDDEN);
  s->info=Text(s,group,"",27,lv_color_white());lv_obj_set_width(s->info,LV_PCT(95));
  lv_obj_add_flag(s->info,LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_text_align(s->info,LV_TEXT_ALIGN_CENTER,0);
  lv_label_set_long_mode(s->info,LV_LABEL_LONG_DOT);lv_obj_set_height(s->info,Px(s,150));
  if(s->theme.minimal) {lv_obj_add_flag(s->info,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(s->date,LV_OBJ_FLAG_HIDDEN);}
  Refresh(s);lv_obj_update_layout(group);
  const int available=s->h-status-Px(s,s->landscape?240:450)-
      (s->theme.date_placement==2&&s->theme.date_style!=4?lv_obj_get_height(s->date)+Px(s,60):0);
  const int group_height=lv_obj_get_height(group);
  const int top=std::max(Px(s,30),std::min(available-group_height,
      s->theme.placement==0?Px(s,110):s->theme.placement==1?(available-group_height)/2:available-group_height));
  lv_obj_align(group,LV_ALIGN_TOP_MID,0,top);
  auto *slider=lv_obj_create(content);s->slider=slider;Clear(slider);
  const int sw=std::min(s->w-Px(s,180),Px(s,1000)),sh=Px(s,136);
  lv_obj_set_size(slider,sw,sh);lv_obj_align(slider,LV_ALIGN_BOTTOM_MID,0,-Px(s,s->landscape?90:210));
  lv_obj_set_style_radius(slider,sh/2,0);lv_obj_set_style_bg_color(slider,Color(0x202328),0);
  lv_obj_set_style_bg_opa(slider,s->theme.minimal?LV_OPA_TRANSP:LV_OPA_90,0);
  const char *hint=s->theme.unlock==0?"Slide to unlock":s->theme.unlock==1?"Swipe up to unlock":"Hold to unlock";
  s->hint=Text(s,slider,hint,32,lv_color_white());lv_obj_center(s->hint);
  if(s->theme.unlock==2) {
    s->hold_progress=lv_obj_create(slider);Clear(s->hold_progress);
    lv_obj_set_size(s->hold_progress,1,Px(s,6));
    lv_obj_set_pos(s->hold_progress,Px(s,40),sh-Px(s,22));
    lv_obj_set_style_radius(s->hold_progress,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_bg_color(s->hold_progress,kAccent,0);
    lv_obj_set_style_bg_opa(s->hold_progress,LV_OPA_COVER,0);
    lv_obj_remove_flag(s->hold_progress,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s->hold_progress,LV_OBJ_FLAG_HIDDEN);
  }
  if(s->theme.unlock==0) {
    s->travel=sw-sh;
    s->knob=lv_obj_create(slider);Clear(s->knob);lv_obj_set_size(s->knob,sh,sh);
    lv_obj_set_style_radius(s->knob,sh/2,0);lv_obj_set_style_bg_color(s->knob,kAccent,0);
    lv_obj_set_style_bg_opa(s->knob,s->theme.minimal?LV_OPA_TRANSP:LV_OPA_COVER,0);
    auto *arrow=Label(s->knob,LV_SYMBOL_RIGHT,UiFont(&lv_font_montserrat_32),kOnAccent);
    lv_obj_center(arrow);lv_obj_remove_flag(s->knob,LV_OBJ_FLAG_CLICKABLE);
  }
  if(!preview && !ambient) {
    auto *touch=s->theme.unlock==1?content:slider;lv_obj_add_flag(touch,LV_OBJ_FLAG_CLICKABLE);
    if(s->theme.unlock==1) {Passive(group);Passive(slider);}
    for(auto event:{LV_EVENT_PRESSED,LV_EVENT_PRESSING,LV_EVENT_RELEASED,LV_EVENT_PRESS_LOST})
      lv_obj_add_event_cb(touch,Touch,event,s);
  }
  // Sweep smoothly while awake; ambient mode retains its aligned one-second ticks.
  s->timer=lv_timer_create([](lv_timer_t *timer){Refresh(static_cast<LockState *>(lv_timer_get_user_data(timer)));},s->theme.clock==1&&!ambient?33:1000,s);
  if(ambient)Refresh(s); // Align the very first tick to the next real second.
  if(ambient){lv_obj_add_flag(slider,LV_OBJ_FLAG_HIDDEN);Passive(root);}
  auto shortcut=[&](const char *name,bool right,Handler action) {
    auto *button=Button(content,name,preview?Handler{}:action);
    lv_obj_set_style_text_font(lv_obj_get_child(button,0),fonts::DisplayFont(Px(s,28)),0);
    lv_obj_set_size(button,Px(s,310),Px(s,90));
    lv_obj_align(button,right?LV_ALIGN_BOTTOM_RIGHT:LV_ALIGN_BOTTOM_LEFT,right?-Px(s,80):Px(s,80),-Px(s,32));
  };
  if(!s->theme.minimal && s->theme.flashlight && (preview||RecoveryFlashlightSupported()))
    shortcut("Flashlight",false,[]{RecoverySetFlashlight(!RecoveryFlashlightEnabled());});
  if(!s->theme.minimal && s->theme.screenshot)
    shortcut("Screenshot",true,[s]{if(s->callback)s->callback(Action::kLockScreenshot,s->context);});
  if(preview) {
    Passive(root);
    lv_obj_add_flag(root,LV_OBJ_FLAG_EVENT_BUBBLE);
  }
  lv_obj_move_foreground(root);
  return root;
}
} // namespace aeraui
