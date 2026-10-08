#include "lrdp/audio/devices.hpp"
#include <chrono>
#include <iostream>
#include <poll.h>
#include <unistd.h>
using namespace lrdp;
int main(){
 try{
  auto devices=make_pipewire_audio({true,true});devices->enable_playback(true);devices->enable_microphone(true);
  std::cout<<"READY "<<getpid()<<'\n'<<std::flush;
  Bytes mono(960*2);for(std::size_t n=0;n<mono.size();n+=2){mono[n]=0x34;mono[n+1]=0x12;}
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);auto next=std::chrono::steady_clock::now();
  unsigned matched=0;
  while(std::chrono::steady_clock::now()<deadline){
   devices->check();const auto now=std::chrono::steady_clock::now();
   if(now>=next){devices->feed_microphone(mono);next=now+std::chrono::milliseconds(20);}
   if(auto stereo=devices->take_playback(960))for(std::size_t i=0;i+3<stereo->size();i+=4){
    const unsigned left=(*stereo)[i]|unsigned((*stereo)[i+1])<<8,right=(*stereo)[i+2]|unsigned((*stereo)[i+3])<<8;
    if(left>=0x1233 && left<=0x1235 && right>=0x1233 && right<=0x1235)++matched;
   }
   if(matched>=1920){devices->enable_microphone(false);devices->enable_playback(false);
    std::cout<<"PASS: real PipeWire virtual microphone -> linked stereo speakers, matched "<<matched<<" PCM frames\n";return 0;}
   ::poll(nullptr,0,2);
  }
  throw ProtocolError("PipeWire did not circulate the expected PCM samples");
 }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
