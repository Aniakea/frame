#ifndef CONTEXT_HH
#define CONTEXT_HH
#pragma once
// 事件循环上下文
class context {
public:
  context() = default;
  ~context() noexcept = default;

  void run() noexcept;
};
#endif // CONTEXT_HH