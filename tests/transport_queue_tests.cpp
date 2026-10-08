#include "lrdp/transport_queue.hpp"
#include <iostream>
using namespace lrdp;
namespace {
void check(bool b,const char* s){if(!b)throw std::runtime_error(s);}
template<class F>void rejects(F f){try{f();}catch(const ProtocolError&){return;}throw std::runtime_error("expected rejection");}
}
int main(){
 try{
  TransportQueue queue;queue.enqueue({Bytes{1,2,3},Bytes{4}});
  const auto active=queue.peek();const auto* identity=active.data();
  queue.enqueue({Bytes{9},Bytes{8}},true);
  check(queue.peek().data()==identity && queue.peek()[0]==1,"TLS retry preserves active pointer despite urgent enqueue");
  queue.consume(1);check(queue.peek()[0]==2 && queue.peek().size()==2,"partial packet remains active");
  queue.consume(2);check(queue.peek()[0]==9,"media preempts only at a packet boundary");queue.consume(1);
  check(queue.peek()[0]==8,"media FIFO order");queue.consume(1);check(queue.peek()[0]==4,"ordinary packet follows media");queue.consume(1);
  check(queue.queued()==0 && queue.peek().empty(),"queue accounting drains to zero");
  queue.enqueue({Bytes{42}});std::vector<Bytes> media(20,Bytes{7});queue.enqueue(std::move(media),true);
  unsigned count=0;while(queue.peek()[0]==7){queue.consume(1);++count;}
  check(count==8 && queue.peek()[0]==42,"fair scheduling prevents permanent graphics starvation");
  rejects([&]{queue.consume(2);});
  TransportQueue bounded;rejects([&]{bounded.enqueue({Bytes(1024*1024+1)},true);});
  check(bounded.queued()==0,"rejected batches do not change queue accounting");
  bounded.enqueue({Bytes(1024*1024)},true);rejects([&]{bounded.enqueue({Bytes{1}},true);});
  rejects([&]{TransportQueue empty;empty.enqueue({Bytes{}});});
  std::cout<<"PASS: packet-boundary audio priority, stable TLS retry buffers, partial completions, fair scheduling and atomic quota rejection\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
