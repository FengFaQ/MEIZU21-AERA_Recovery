/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
namespace aeraui::mouse_cursor {
constexpr int kWidth=44, kHeight=62;
inline std::array<uint32_t,kWidth*kHeight> Render(uint32_t accent) {
  constexpr float px[]={2,2,13,23,34,24,41};
  constexpr float py[]={2,47,37,59,53,33,31};
  std::array<uint32_t,kWidth*kHeight> pixels{};
  for(int y=0;y<kHeight;y++)for(int x=0;x<kWidth;x++) {
    unsigned filled=0,edge=0;
    for(int sy=0;sy<2;sy++)for(int sx=0;sx<2;sx++) {
      float fx=x+(sx+.5f)/2,fy=y+(sy+.5f)/2;
      bool inside=false;float distance=10000;
      for(int i=0,j=6;i<7;j=i++) {
        if(((py[i]>fy)!=(py[j]>fy)) &&
           fx<(px[j]-px[i])*(fy-py[i])/(py[j]-py[i])+px[i])inside=!inside;
        float dx=px[j]-px[i],dy=py[j]-py[i];
        float t=std::clamp(((fx-px[i])*dx+(fy-py[i])*dy)/(dx*dx+dy*dy),0.f,1.f);
        float ex=fx-px[i]-t*dx,ey=fy-py[i]-t*dy;
        distance=std::min(distance,ex*ex+ey*ey);
      }
      if(inside) {filled++;if(distance<=9)edge++;}
    }
    if(filled) {
      unsigned r=((accent>>16)&255)*edge/filled,g=((accent>>8)&255)*edge/filled,b=(accent&255)*edge/filled;
      pixels[y*kWidth+x]=(filled*255/4<<24)|(r<<16)|(g<<8)|b;
    }
  }
  return pixels;
}
}
