#ifndef LOADER_HH
#define LOADER_HH
#pragma once

class context;

namespace loader {
class ui {
public:
  ui() = default;
  ~ui() noexcept = default;

  void set_context(context *ctx);
};

class service {
public:
  service() = default;
  ~service() noexcept = default;

  void set_context(context *ctx);
};
} // namespace loader

#endif // LOADER_HH