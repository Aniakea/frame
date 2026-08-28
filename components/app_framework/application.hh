#ifndef APPLICATION_HH
#define APPLICATION_HH
#pragma once

class impl;

class application {
public:
  application();
  ~application() noexcept;

  void run();

private:
  impl *pimpl_;
};

#endif // APPLICATION_HH