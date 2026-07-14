#include <iostream>

#include "base/request.h"

using namespace Ramulator;

int main() {
  ReqBuffer buffer;
  buffer.max_size = 2;
  Request first(0, Request::Type::Read);
  Request second(32, Request::Type::Read);
  Request overflow(64, Request::Type::Read);
  if (!buffer.enqueue(first) || !buffer.enqueue(second) ||
      buffer.size() != 2 || buffer.enqueue(overflow) || buffer.size() != 2) {
    std::cerr << "FAIL: ReqBuffer exceeded or misreported its hard capacity\n";
    return 1;
  }
  std::cout << "ReqBuffer hard-capacity regression: PASS\n";
  return 0;
}
